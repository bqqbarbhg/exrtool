
#include <stdlib.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_BOOL
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#include "ext/nuklear.h"

#include "ext/tinyfiledialogs.h"
#include "ext/tinyexr.h"
#include "exrtool.h"

#include <vector>
#include <string>
#include <memory>
#include <map>
#include <regex>

#include <stdio.h>
#include <stdarg.h>

void platformPing();

static const constexpr nk_flags aMidLeft = NK_TEXT_ALIGN_MIDDLE | NK_TEXT_ALIGN_LEFT;
static const constexpr nk_flags aMidRight = NK_TEXT_ALIGN_MIDDLE | NK_TEXT_ALIGN_RIGHT;

struct EXRHeaderDeleter { void operator()(EXRHeader *p) { FreeEXRHeader(p); delete p; } };
struct EXRImageDeleter { void operator()(EXRImage *p) { FreeEXRImage(p); delete p; } };

void error(const char *fmt, ...)
{
	char buf[1024];

	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	tinyfd_messageBox("Error", buf, "ok", "error", 0);
}

struct UIFile
{
	std::string name;

	std::unique_ptr<EXRHeader, EXRHeaderDeleter> header;

	EXRHeader *getHeader()
	{
		if (header) return header.get();

		int ret = 0;

		EXRVersion version;
		ret = ParseEXRVersionFromFile(&version, name.c_str());
		if (ret) {
			error("Could not parse version\n%s", name.c_str());
			return nullptr;
		}

		const char *err;

		EXRHeader *pHeader = new EXRHeader();
		ret = ParseEXRHeaderFromFile(pHeader, &version, name.c_str(), &err);
		if (ret) {
			error("Could not parse version\n%s\n%s", err, name.c_str());
			FreeEXRErrorMessage(err);
			return nullptr;
		}

		header.reset(pHeader);

		return header.get();
	}
};

struct CategoryDesc
{
	std::string name;
	std::regex pattern;
};

std::vector<CategoryDesc> &getCategories()
{
	static std::vector<CategoryDesc> categories = []() {
		std::vector<CategoryDesc> c;
		auto r = [](const char *src) -> std::regex { return std::regex{ src, std::regex_constants::ECMAScript }; };

		c.push_back({ "Color (Beauty)", r("[RGBA]") });
		c.push_back({ "Normal (N)", r("N\\.[XYZ]") });
		c.push_back({ "Depth (Z)", r("Z") });
		c.push_back({ "Ambient Occlusion (AO)", r("AO\\.[RGBA]") });
		c.push_back({ "Crypto Object", r("crypto_object.*") });
		c.push_back({ "Crypto Material", r("crypto_material.*") });
		c.push_back({ "Sample density", r("AA_inv_density.*") });
		c.push_back({ "Variance", r("variance.*") });
		c.push_back({ "Noice", r(".*noice.*") });
		c.push_back({ "Others", r(".*") });

		return c;
	}();
	return categories;
}

struct Category
{
	std::string name;
	bool open = false;
	std::map<std::string, bool> channels;
};

struct UIFileList
{
	UIFileList() { }
	UIFileList(const UIFileList&) = delete;
	UIFileList& operator=(const UIFileList&) = delete;
	UIFileList(UIFileList&&) = default;
	UIFileList& operator=(UIFileList&&) = default;

	std::string name;
	std::vector<UIFile> files;
	std::vector<Category> categories;
};

struct UIState
{
	nk_context *ctx;
	std::vector<UIFileList> fileLists;
	int selectedIndex = -1;

	exrtool_run *tool_run = nullptr;

	int width = 0, height = 0;

	std::string commonPrefix;

	UIState(nk_context *ctx) : ctx(ctx)
	{
	}

	void addFile(const std::vector<std::string> &names)
	{
		UIFileList list;

		for (const std::string &name : names) {
			UIFile file;
			file.name = name;
			list.files.push_back(std::move(file));
		}

		list.name = list.files[0].name;

		if (fileLists.size() == 0) {
			commonPrefix = list.name;
		}

		size_t bestIndex = 0;
		for (size_t i = 0; i < list.name.size(); i++) {
			if (i >= commonPrefix.size()) break;
			if (list.name[i] != '\\' && list.name[i] != '/') continue;
			if (memcmp(commonPrefix.data(), list.name.data(), i + 1) != 0) break;
			bestIndex = i + 1;
		}

		commonPrefix = commonPrefix.substr(0, bestIndex);

		EXRHeader *header = list.files[0].getHeader();
		if (!header) return;

		std::vector<bool> matched;
		matched.resize(header->num_channels);

		auto &descs = getCategories();
		for (CategoryDesc &desc : descs) {
			Category cat;

			for (int i = 0; i < header->num_channels; i++) {
				if (matched[i]) continue;

				EXRChannelInfo &ch = header->channels[i];
				if (std::regex_match(ch.name, desc.pattern)) {
					cat.channels[ch.name] = false;
					matched[i] = true;
				}
			}

			if (!cat.channels.empty()) {
				cat.name = desc.name;
				list.categories.push_back(std::move(cat));
			}
		}

		fileLists.push_back(std::move(list));
	}

	void uiChannelList(UIFileList &list)
	{
		int itemHeight = 22;

		EXRHeader *header = list.files[0].getHeader();

		int numItems = (int)list.categories.size();
		for (Category &cat : list.categories) {
			if (cat.open) {
				numItems += (int)cat.channels.size();
			}
		}

		nk_list_view vlist;
		if (nk_list_view_begin(ctx, &vlist, "Channels", 0, itemHeight, numItems)) {

			int ix = 0;
			for (Category &cat : list.categories) {
				bool open = cat.open;

				if (ix >= vlist.begin && ix < vlist.end) {
					nk_layout_row_begin(ctx, NK_DYNAMIC, (float)itemHeight, 2);

					float ratio = 35.0f / (width / 2);

					nk_layout_row_push(ctx, ratio);
					if (nk_button_symbol(ctx, cat.open ? NK_SYMBOL_MINUS : NK_SYMBOL_PLUS)) {
						cat.open = !open;
					}

					uint32_t num = 0;
					for (auto &pair : cat.channels) {
						if (pair.second) num++;
					}

					char label[128];
					snprintf(label, sizeof(label), "%s %u/%u", cat.name.c_str(),
						num, (uint32_t)cat.channels.size());

					bool all = num == cat.channels.size();

					nk_layout_row_push(ctx, 1.0f - ratio);
					if (nk_checkbox_label(ctx, label, &all)) {
						for (auto &pair : cat.channels) {
							pair.second = all;
						}
					}

					nk_layout_row_end(ctx);
				}
				ix++;

				if (open) {
					for (auto &pair : cat.channels) {

						if (ix >= vlist.begin && ix < vlist.end) {
							nk_layout_row_dynamic(ctx, (float)itemHeight, 1);
							nk_checkbox_label(ctx, pair.first.c_str(), &pair.second);
						}

						ix++;
					}
				}
			}

			nk_list_view_end(&vlist);
		}
	}

	void updateMain()
	{
		if (!nk_begin(ctx, "main", nk_recti(0, 0, width, height), 0)) return;

		int menuHeight = 22;

		nk_layout_row_begin(ctx, NK_STATIC, 22.0f, 3);

		nk_layout_row_push(ctx, 130.0f);
		if (nk_button_label(ctx, "Add sequence")) {
			const char *filters[] = { "*.exr" };
			const char *file = tinyfd_openFileDialog("Add source file", nullptr, 1, filters, ".exr files", 1);
			std::vector<std::string> names;
			while (file) {
				const char *end = strchr(file, '|');
				names.push_back(end ? std::string(file, end) : std::string(file));
				file = end ? end + 1 : NULL;
			}

			if (names.size() > 0) {
				addFile(names);
			}
		}

		nk_layout_row_push(ctx, 70.0f);
		if (nk_button_label(ctx, "Save")) {
			const char *filters[] = { "*.exr" };
			const char *output = tinyfd_saveFileDialog("Save modified files", nullptr, 1, filters, ".exr files");
			if (output) {
				std::vector<std::vector<const char *>> list_channels;
				std::vector<exrtool_file> files;

				for (UIFileList &list : fileLists) {
					std::vector<const char *> channels;
					for (Category &cat : list.categories) {
						for (auto &pair : cat.channels) {
							if (!pair.second) continue;
							channels.push_back(pair.first.c_str());
						}
					}

					for (UIFile &file : list.files) {
						exrtool_file ef;

						ef.name = file.name.c_str();
						ef.channels = channels.data();
						ef.num_channels = channels.size();

						files.push_back(ef);
					}

					list_channels.push_back(std::move(channels));
				}

				exrtool_input input = { };
				input.files = files.data();
				input.num_files = files.size();
				input.output_file = output;
				input.progress_fn = [](exrtool_run*, void*) {
					platformPing();
				};
				tool_run = exrtool_process(&input);
			}
		}

		nk_layout_row_push(ctx, 80.0f);
		if (nk_button_label(ctx, "Reset")) {
			fileLists.clear();
			selectedIndex = -1;
			commonPrefix.clear();
		}

		nk_layout_row_end(ctx);

		nk_layout_row_dynamic(ctx, (float)height - menuHeight*2, 2);

		int itemHeight = 22;

		nk_list_view vlist;
		if (nk_list_view_begin(ctx, &vlist, "Files", 0, itemHeight, (int)fileLists.size())) {

			for (int i = vlist.begin; i < vlist.end; i++) {
				UIFileList &list = fileLists[i];

				nk_layout_row_dynamic(ctx, (float)itemHeight, 1);

				nk_bool selected = selectedIndex == i;
				nk_selectable_label(ctx, list.name.c_str() + commonPrefix.size(), aMidLeft, &selected);
				if (selected) selectedIndex = i;
			}

			nk_list_view_end(&vlist);
		}

		if (selectedIndex >= 0 && selectedIndex < fileLists.size()) {
			uiChannelList(fileLists[selectedIndex]);
		}

		nk_end(ctx);
	}

	void updateProgress(exrtool_progress progress)
	{
		if (!nk_begin(ctx, "main", nk_recti(0, 0, width, height), 0)) return;

		nk_layout_row_dynamic(ctx, 40.0f, 1);
		nk_label(ctx, "Processing", NK_TEXT_CENTERED);

		nk_layout_row_dynamic(ctx, 40.0f, 1);
		nk_progress(ctx, &progress.done, progress.max, false);

		nk_end(ctx);
	}

	void update(int w, int h)
	{
		width = w;
		height = h;

		if (tool_run) {
			exrtool_progress progress;
			bool done = exrtool_poll(tool_run, &progress);

			if (done) {
				const char *err = exrtool_get_error(tool_run, 0);
				if (err) error("Processing error\n%s", err);
				exrtool_free(tool_run);
				tool_run = nullptr;

				updateMain();
			} else {
				updateProgress(progress);
			}

		} else {
			updateMain();
		}
	}
};

UIState *uiInit(nk_context *ctx)
{
	return new UIState(ctx);
}

void uiUpdate(UIState *state, int width, int height)
{
	state->update(width, height);
}
