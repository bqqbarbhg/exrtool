#include "exrtool.h"
#include <stdint.h>
#include <ctype.h>

#include <vector>
#include <map>
#include <unordered_set>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

#include <stdio.h>
#include <stdarg.h>

#include "ext/tinyexr.h"

static uint32_t strip_frame(const char *str)
{
	const char *end = str + strlen(str);
	while (end > str && !isdigit(end[-1])) end--;
	const char *begin = end;
	while (begin > str && isdigit(begin[-1])) begin--;
	if (begin == end) return ~0u;

	return (uint32_t)atoi(begin);
}

struct exrtool_run_file
{
	std::string name;
	std::unordered_set<std::string> channels;

	bool use_channel(const char *channel) const
	{
		return channels.count(channel) > 0;
	}
};

struct exrtool_run
{
	std::vector<std::pair<uint32_t, std::vector<exrtool_run_file>>> frames;
	std::string output_name;
	exrtool_input input;

	std::atomic_uint32_t a_frames_started;
	std::atomic_uint32_t a_progress;
	std::atomic_uint32_t a_threads_done;

	std::vector<std::thread> threads;

	std::mutex error_mutex;
	std::vector<std::string> errors;

	void error(const char *fmt, ...)
	{
		char buf[1024];

		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);

		std::lock_guard<std::mutex> lg(error_mutex);
		errors.push_back(buf);
	}

};

bool process_frame(exrtool_run &run, uint32_t frame, const std::vector<exrtool_run_file> &files)
{
	std::vector<EXRHeader> headers;
	std::vector<EXRImage> images;

	std::vector<EXRChannelInfo> channels;
	std::vector<unsigned char*> datas;

	bool ok = true;

	for (const exrtool_run_file &file : files) {
		int ret;
		const char *err = nullptr;
		EXRVersion version;

		ret = ParseEXRVersionFromFile(&version, file.name.c_str());
		if (ret) {
			run.error("Failed to parse EXR version\n%s", file.name);
			ok = false;
			break;
		}

		EXRHeader header;
		ret = ParseEXRHeaderFromFile(&header, &version, file.name.c_str(), &err);
		if (ret) {
			run.error("Failed to parse EXR header\n%s\n%s", file.name, err);
			FreeEXRErrorMessage(err);
			ok = false;
			break;
		}

		EXRImage image;
		InitEXRImage(&image);

		ret = LoadEXRImageFromFile(&image, &header, file.name.c_str(), &err);
		if (ret) {
			run.error("Failed to load EXR image\n%s\n%s", file.name, err);
			FreeEXRHeader(&header);
			FreeEXRErrorMessage(err);
			ok = false;
			break;
		}

		run.a_progress.fetch_add(1, std::memory_order_relaxed);

		for (size_t i = 0; i < header.num_channels; i++) {
			EXRChannelInfo &chan = header.channels[i];
			if (!file.use_channel(chan.name)) continue;
			unsigned char *data = image.images[i];

			auto it = std::lower_bound(channels.begin(), channels.end(), chan,
				[](const EXRChannelInfo &lhs, const EXRChannelInfo &rhs) {
				return strcmp(lhs.name, rhs.name) < 0;
			});

			size_t offset = it - channels.begin();
			if (it != channels.end() && !strcmp(it->name, chan.name)) {
				*it = chan;
				datas[offset] = data;
			} else {
				channels.insert(it, chan);
				datas.insert(datas.begin() + offset, data);
			}
		}

		headers.push_back(header);
		images.push_back(image);
	}

	if (ok && channels.size() == 0) {
		run.error("Frame %u has no channels", frame);
		ok = false;
	}

	if (ok) {
		EXRHeader header = headers[0];
		EXRImage image = images[0];

		std::vector<int> channel_types;
		channel_types.reserve(channels.size());
		for (EXRChannelInfo &chan : channels) {
			channel_types.push_back(chan.pixel_type);
		}

		header.channels = channels.data();
		header.pixel_types = channel_types.data();
		header.requested_pixel_types = channel_types.data();
		header.num_channels = (int)channels.size();
		image.images = datas.data();
		image.num_channels = (int)datas.size();

		std::string name = run.output_name;
		size_t end = name.find_last_of('#');
		if (frame != ~0u && end != std::string::npos) {
			size_t begin = end;
			while (begin > 0 && name[begin - 1] == '#') begin--;

			size_t num = end - begin + 1;
			char buf[32];
			snprintf(buf, sizeof(buf), "%0*u", (int)num, frame);
			name.replace(begin, num, buf);
		}

		int ret;
		const char *err = nullptr;
		ret = SaveEXRImageToFile(&image, &header, name.c_str(), &err);

		if (ret) {
			run.error("Failed to save EXR image\n%s\n%s", name, err);
			FreeEXRErrorMessage(err);
			ok = false;
		}
	}

	run.a_progress.fetch_add(1, std::memory_order_relaxed);

	for (EXRHeader &header : headers) {
		FreeEXRHeader(&header);
	}
	for (EXRImage &image : images) {
		FreeEXRImage(&image);
	}

	return ok;
}

bool process_next_frame(exrtool_run &run)
{
	uint32_t ix = run.a_frames_started.fetch_add(1, std::memory_order_relaxed);
	if (ix >= run.frames.size()) return false;

	auto &pair = run.frames[ix];
	return process_frame(run, pair.first, pair.second);
}

#ifdef __cplusplus
extern "C" {
#endif

exrtool_run *exrtool_process(const exrtool_input *input)
{
	std::map<uint32_t, std::vector<exrtool_run_file>> frames;
	bool ok = true;

	for (size_t i = 0; i < input->num_files; i++) {
		const exrtool_file *file = &input->files[i];
		uint32_t frame = strip_frame(file->name);

		exrtool_run_file rf;
		rf.name = file->name;
		rf.channels.reserve(file->num_channels);
		for (size_t i = 0; i < file->num_channels; i++) {
			rf.channels.insert(file->channels[i]);
		}

		frames[frame].push_back(rf);
	}

	exrtool_run *run = new exrtool_run();
	run->frames = decltype(run->frames)(frames.begin(), frames.end());
	run->output_name = input->output_file;
	run->input = *input;

	size_t num_threads = input->num_threads;
	if (num_threads == 0) {
		size_t cores = std::thread::hardware_concurrency();
		if (cores > 2) {
			num_threads = cores - 2;
		} else {
			num_threads = 1;
		}
	}

	for (size_t i = 0; i < num_threads; i++) {
		run->threads.emplace_back([=](){
			while (process_next_frame(*run)) {
				if (run->input.progress_fn) {
					run->input.progress_fn(run, run->input.progress_user);
				}
			}
			run->a_threads_done.fetch_add(1, std::memory_order_release);
			if (run->input.progress_fn) {
				run->input.progress_fn(run, run->input.progress_user);
			}
		});
	}

	return run;
}

bool exrtool_poll(exrtool_run *run, exrtool_progress *progress)
{
	if (progress) {
		progress->done = run->a_progress.load(std::memory_order_relaxed);
		progress->max = run->input.num_files + run->frames.size();
	}
	return run->a_threads_done.load(std::memory_order_acquire) == run->threads.size();
}

size_t exrtool_get_num_errors(exrtool_run *run)
{
	std::lock_guard<std::mutex> lg(run->error_mutex);
	return run->errors.size();
}

const char *exrtool_get_error(exrtool_run *run, size_t index)
{
	std::lock_guard<std::mutex> lg(run->error_mutex);
	if (index >= run->errors.size()) return nullptr;
	return run->errors[index].c_str();
}

void exrtool_free(exrtool_run *run)
{
	for (auto &thread : run->threads) {
		thread.join();
	}
	delete run;
}

#ifdef __cplusplus
}
#endif
