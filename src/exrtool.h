#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct exrtool_run exrtool_run;
typedef void (*exrtool_progress_fn)(exrtool_run *run, void *user);

typedef struct exrtool_file {
	const char *name;
	const char **channels;
	size_t num_channels;
} exrtool_file;

typedef struct exrtool_input {

	const char *output_file;

	const exrtool_file *files;
	size_t num_files;

	size_t num_threads;

	exrtool_progress_fn progress_fn;
	void *progress_user;

} exrtool_input;

typedef struct exrtool_progress {
	size_t done;
	size_t max;
} exrtool_progress;

exrtool_run *exrtool_process(const exrtool_input *input);
bool exrtool_poll(exrtool_run *run, exrtool_progress *progress);
size_t exrtool_get_num_errors(exrtool_run *run);
const char *exrtool_get_error(exrtool_run *run, size_t index);
void exrtool_free(exrtool_run *run);

#ifdef __cplusplus
}
#endif
