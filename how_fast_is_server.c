/*
  If you intend to use a SSL-based protocol here you might need to setup TLS
  library mutex callbacks as described here
  https://curl.se/libcurl/c/threadsafe.html
*/

#include <curl/curl.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#ifndef WIN32
#include <unistd.h>
#endif

extern char *optarg;
extern int opterr, optind, optopt;

/****
	This line is used during the thread creation process below
		nanosleep((const struct timespec[]){{0, WAIT_TIME_NANO}}, NULL);

	It uses an anonymous timespec struct using only nanoseconds
	The 0 in the timespec is the seconds field
	The max value for nanoseconds is 999999999L
	The below WAIT_TIME_NANO definition is quite small
	and it works on my Ubuntu dev system with a max of 30K threads per process
	anything faster than 100000L creates connection problems on my system

	CURL error 6 or 7 become common when there are high numbers of (threads)number_of_runs

	On Linux systems find the number of threads possible and use as a high end for -r
	backoff a few hundred to find the sweet spot that results in non-error running
	I use about 30K as input to -r
	cat /proc/sys/kernel/threads-max
****/
#define WAIT_TIME_NANO 150000L
# define timespec_diff_macro(a, b, result)                  \
  do {                                                \
    (result)->tv_sec = (a)->tv_sec - (b)->tv_sec;     \
    (result)->tv_nsec = (a)->tv_nsec - (b)->tv_nsec;  \
    if ((result)->tv_nsec < 0) {                      \
      --(result)->tv_sec;                             \
      (result)->tv_nsec += 1000000000;                \
    }                                                 \
  } while (0)

#define handle_error_en(en, msg) \
		do { errno = en; perror(msg); exit(EXIT_FAILURE); } while (0)

typedef struct configuration {
	char* program_name;
	char* url;
	long number_of_runs;
	int silent;
	struct timespec program_start;
	struct timespec program_end;
} configuration;

struct thread_info {
	pthread_t		thread_id;
	int				thread_num;
	char*			url;
	struct timespec start;
	struct timespec end;
	long			response_code;
	long			os_error_code;
	long			curl_error_code;
	char			curl_error_string[CURL_ERROR_SIZE];
	curl_off_t		curl_total;

};

// an empty body is written do avoid results of each run being printed to the screen
static size_t write_curl_data(void *data, size_t size, size_t nmemb, void *userp) {
	return nmemb;
}

static void* start_curl_run(void* arg) {
	struct thread_info *this_thread = arg;
	timespec_get(&this_thread->start, TIME_UTC);
	CURL *curl = curl_easy_init();
	if(curl) {
		CURLcode curl_result;
		this_thread->curl_error_string[0] = 0;
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, &this_thread->curl_error_string);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_curl_data);
		curl_easy_setopt(curl, CURLOPT_URL, this_thread->url);
		curl_result = curl_easy_perform(curl);
		this_thread->curl_error_code = curl_result;
		curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &this_thread->curl_total);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &this_thread->response_code);
		curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &this_thread->os_error_code);
  		curl_easy_cleanup(curl);
	}
	else {
		fprintf(stderr, "unable to initialize curl object for thread_number: %d", this_thread->thread_num);
	}
	timespec_get(&this_thread->end, TIME_UTC);
}

void usage(char* argv) {
	printf("Usage: %s -u URL -r number of runs ][-s produce less output]\n", argv);
	printf("Pipe through tee to create a logfile\n");
	printf("\t%s -u http://localhost -r 30000 | tee full.log\n", argv);
	printf("\t%s -u http://localhost -r 30000 -s | tee error.log\n", argv);
	exit(1);
}

int validate_configuration(const configuration* config) {
	return (!config->program_name || !config->url || !config->number_of_runs) ? 0 : 1;
}

int main(int argc, char **argv) {
	struct configuration config = {program_name: argv[0], number_of_runs: 0, silent: 0};
	int opt;
    while ((opt = getopt(argc, argv, ":u:r:ols")) != -1) {
        switch(opt) {
			case 'u': config.url = optarg; break;
        	case 'r': config.number_of_runs = strtoul(optarg, NULL, 0); break;
			case 's': config.silent = 1; break;
        }
    }

	if(!validate_configuration(&config)) usage(config.program_name);
	timespec_get(&config.program_start, TIME_UTC);
	struct thread_info *threads = calloc(config.number_of_runs, sizeof(struct thread_info));
	curl_global_init(CURL_GLOBAL_ALL);

	int result;
	int_fast64_t i;
	for(i = 0; i < config.number_of_runs; i++) {
		threads[i].thread_num = i;
		threads[i].url = config.url;
		result = pthread_create(&threads[i].thread_id, NULL, start_curl_run, &threads[i]);
		if(result) {
			char temp_error_buf[50];
			sprintf(temp_error_buf, "pthread_create: thread_num %ld", i);
			handle_error_en(result, temp_error_buf);
		}
		// See commet at the top of this file
		nanosleep((const struct timespec[]){{0, WAIT_TIME_NANO}}, NULL);
	}

	int errors = 0;
 	void* response;
	for(i = 0; i < config.number_of_runs; i++) {
		result = pthread_join(threads[i].thread_id, &response);
		if(result) {
			char temp_error_buf[50];
			sprintf(temp_error_buf, "pthread_join: thread_num %ld", i);
			handle_error_en(result, temp_error_buf);
		}
		struct timespec elapsed_time;
		timespec_diff_macro(&threads[i].end, &threads[i].start, &elapsed_time);
		if(threads[i].curl_error_code) ++errors;
		if(!config.silent || threads[i].curl_error_code) {
			char* nl = (strrchr(threads[i].curl_error_string, '\n')) ? "" : "\n";
			fprintf(stdout, "Thread=%d: response_code=%ld: seconds=%ld.%09ld: curl_time_t=%06ld: os_error_code=%ld: curl_error_code=%ld: curl_error=%s%s",
							threads[i].thread_num, threads[i].response_code,
							elapsed_time.tv_sec, elapsed_time.tv_nsec,
							threads[i].curl_total, threads[i].os_error_code,
							threads[i].curl_error_code, threads[i].curl_error_string, nl);
		}
	}
	curl_global_cleanup();
	free(threads);
	timespec_get(&config.program_end, TIME_UTC);
	struct timespec elapsed_program_time;
	timespec_diff_macro(&config.program_end, &config.program_start, &elapsed_program_time);
	printf("%d errors out of %ld runs in %ld.%09ld real seconds\n", errors, config.number_of_runs, elapsed_program_time.tv_sec, elapsed_program_time.tv_nsec);
	return 0;
}
