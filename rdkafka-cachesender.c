#include <ctype.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <syslog.h>
#include <sys/time.h>
#include <errno.h>
#include <assert.h>
#include <stdbool.h>

#define DEFAULT_LINES_TO_SEND 1
#define DEFAULT_LINE_LENGTH 100
#define DEFAULT_CHUNK_SIZE 16
#define ONE_MILLION 1000000
#define USECS_SLEEP_TIME 100
#define MSGS_TO_SEND -1
#define TOL 0.05

#include "rdkafka.h"  /* for Kafka driver */

static volatile bool run = 1;
static rd_kafka_t *rk;

static void print_usage_err ()
{
	fprintf(stderr, "Invalid parameter. Usage:\n\trdkafka_cachesender "
					"-f <filename> -b <brokers> -t <topic> [-p <partition>] "
					"[-l <line_length>] [-n <lines_to_send>] [-s <sleep_time>] [-m <messages_to_send>]");
}

static void stop (int sig)
{
	run = false;
}

static void cleanup(rd_kafka_t *rk, rd_kafka_topic_t *rkt, unsigned long total_msgs_sent)
{
	/* Poll to handle delivery reports */
	rd_kafka_poll(rk, 0);

	fprintf(stdout, "Stopped after producing %lu msgs. Delivering %d unsent msgs...", total_msgs_sent, rd_kafka_outq_len(rk));

	/* Wait for messages to be delivered */
	while (run && rd_kafka_outq_len(rk) > 0)
		rd_kafka_poll(rk, 100);

	fprintf(stdout, " done.\n");

	/* Destroy topic */
	rd_kafka_topic_destroy(rkt);

	/* Destroy the handle */
	rd_kafka_destroy(rk);

	/* Let background threads clean up and terminate cleanly. */
	rd_kafka_wait_destroyed(2000);
}

static void logger (const rd_kafka_t *rk, int level, const char *fac, const char *buf)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	fprintf(stderr, "%u.%03u RDKAFKA-%i-%s: %s: %s\n",
			(int)tv.tv_sec,
			(int)(tv.tv_usec / 1000),
			level, fac, rd_kafka_name(rk), buf);
}

static void msg_delivered (rd_kafka_t *rk, void *payload, size_t len, rd_kafka_resp_err_t error_code, void *opaque, void *msg_opaque)
{
	if (error_code)
		fprintf(stderr, "%% Message delivery failed: %s\n", rd_kafka_err2str(error_code));
}

static void sig_usr1 (int sig)
{
	rd_kafka_dump(stdout, rk);
}

static void errexit(char* const msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(-1);
}

/* ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~
 * main
 * ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~ */
int main (int argc, char **argv)
{

	/* ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~
	 * setup
	 * ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~ */

	rd_kafka_topic_t *rkt;
	int lines_to_send = DEFAULT_LINES_TO_SEND;
	int sleep_time = USECS_SLEEP_TIME;
	unsigned long msgs_to_send = MSGS_TO_SEND;
	bool is_msgs_limited = false;
	int partition = RD_KAFKA_PARTITION_UA;
	int file_size = 0;
	int *offset_table = (int*) malloc(DEFAULT_CHUNK_SIZE);	//TODO free
	int offset_table_size = 0;
	char *memory;

	int opt;
	char *filename_string = NULL;
	char *line_length_string = NULL;
	char *lines_to_send_string = NULL;
	char *sleep_time_string = NULL;
	char *msgs_to_send_string = NULL;
	char *topic = NULL;
	char *brokers = NULL;
	int line_length = DEFAULT_LINE_LENGTH;
	rd_kafka_conf_t *conf = rd_kafka_conf_new();
	rd_kafka_topic_conf_t *topic_conf = rd_kafka_topic_conf_new();
	char errstr[512];

	while ((opt = getopt(argc, argv, "t:p:b:f:l:n:s:m:")) != -1)
	{
		switch (opt)
		{
			case 't':
				topic = optarg;
				break;
			case 'p':
				partition = atoi(optarg);
				break;
			case 'b':
				brokers = optarg;
				break;
			case 'f':
				filename_string = optarg;
				break;
			case 'l':
				line_length_string = optarg;
				break;
			case 'n':
				lines_to_send_string = optarg;
				break;
			case 's':
				sleep_time_string = optarg;
				break;
			case 'm':
				msgs_to_send_string = optarg;
				break;
			default:
				print_usage_err();
				errexit("");
		}
	}

	if(line_length_string != NULL)
	{
		line_length = atoi(line_length_string);
		if(line_length < 1)
			errexit("Invalid line length");
	}

	if(lines_to_send_string != NULL)
	{
		lines_to_send = atoi(lines_to_send_string);
		if(lines_to_send <= 0)
			errexit("Invalid lines to send number");
	}

	if(sleep_time_string != NULL)
	{
		sleep_time = atoi(sleep_time_string);
		if(sleep_time < 0)
			errexit("Invalid sleep time number");
	}

	if(msgs_to_send_string != NULL)
	{
		msgs_to_send = (unsigned long)atol(msgs_to_send_string);
		is_msgs_limited = true;
		if(msgs_to_send < 0)
			errexit("Invalid msgs_to_send number");
	}

	if(filename_string == NULL)
		errexit("A file name should be specified");

	memory = (char*) malloc(DEFAULT_CHUNK_SIZE);	//TODO free
	if(!memory)
		errexit("Cannot allocate memory");

	int mem_size = DEFAULT_CHUNK_SIZE;
	FILE *fptr = fopen(filename_string, "rb");
	if(!fptr)
		errexit("Error opening file");

	while(fptr)
	{
		int retc = fread(memory + file_size, 1, DEFAULT_CHUNK_SIZE, fptr);
		file_size += retc;

		if(retc < DEFAULT_CHUNK_SIZE)
		{
			if(feof(fptr))
			{
				fclose(fptr);
				fptr = NULL;
				break;
			}
			if(ferror(fptr))
				errexit("Error reading file");

			assert(0);
		}

		mem_size += DEFAULT_CHUNK_SIZE;
		memory = (char*) realloc(memory, mem_size);
		if(!memory)
			errexit("Cannot reallocate memory");
	}

	// now parse the memory and divide the content in batch with line_length maximum length.
	if(!offset_table)
		errexit("Cannot allocate memory");

	int curr_line = 0;
	int line_mem_size = DEFAULT_CHUNK_SIZE;
	int lines_sent;
	int last_space_offset = 0;
	int last_line_offset = 0;

	for(lines_sent = 0; lines_sent < file_size; lines_sent++)
	{
		if(lines_sent - last_line_offset >= line_length)
		{
			if(last_space_offset == last_line_offset)
				errexit("Line too long, try increasing line length");

			offset_table[curr_line] = last_space_offset + 1;
			memory[last_space_offset] = '\n';
			curr_line++;
			if(curr_line * sizeof(int) >= line_mem_size)
			{
				line_mem_size += DEFAULT_CHUNK_SIZE;
				offset_table = (int*) realloc(offset_table, line_mem_size);
				if(!offset_table)
					errexit("Cannot reallocate memory");
			}
			last_space_offset ++;
			last_line_offset = last_space_offset;
		}
		if(memory[lines_sent] == ' ')
			last_space_offset = lines_sent;
	}
	if(last_line_offset < file_size)
	{
		offset_table[curr_line] = file_size;
		curr_line ++;
	}

	offset_table_size = curr_line;

	/* Set up a message delivery report callback.
	 * It will be called once for each message, either on successful delivery to broker, or upon failure to deliver to broker. */
	rd_kafka_conf_set_dr_cb(conf, msg_delivered);

	// Create Kafka handle
	if (!(rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr))))
	{
		fprintf(stderr, "%% Failed to create new producer: %s\n", errstr);
		exit(1);
	}

	// Set logger
	rd_kafka_set_logger(rk, logger);
	rd_kafka_set_log_level(rk, LOG_DEBUG);

	// Add brokers
	if (rd_kafka_brokers_add(rk, brokers) == 0)
		errexit("%% No valid brokers specified");

	// Create topic
	rkt = rd_kafka_topic_new(rk, topic, topic_conf);

	printf(	"welcome to Kafka Cachesender! describing configuration\n"
	"\t- producing messages of %d lines * %d B each = %d B\n"
	"\t- sleeping for %d usecs between each message (!!!)\n"
	"\t- sourcing from file %s (%d B)\n"
	"\t- sending to topic %s partition %d of broker %s\n",
		lines_to_send, line_length, lines_to_send*line_length, sleep_time,
		filename_string, file_size, topic, partition, brokers);

	signal(SIGINT, stop);
	signal(SIGUSR1, sig_usr1);

	/* ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~
	 * cache_send
	 * ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~ */

	struct timeval timer_start, timer_end;
	gettimeofday(&timer_start, NULL);
	double throughput_tol = TOL;
	long total_bytes_sent = 0;
	unsigned long total_msgs_sent = 0;
	bool send_more_msgs = true;

	while (run && send_more_msgs)
	{
		int line_offset = 0;
		int lines_sent;

		for(lines_sent = 0; lines_sent < offset_table_size; lines_sent += lines_to_send)
		{
			if(run==false)
				break;

			char *memory_to_send = memory + line_offset;
			int bytes_to_send, index;

			if(lines_sent > offset_table_size - lines_to_send)
				index = offset_table_size;
			else
				index = lines_sent + lines_to_send;
			bytes_to_send = offset_table[index - 1] - line_offset;

			// Send/Produce message
			if (rd_kafka_produce(rkt, partition, RD_KAFKA_MSG_F_COPY,
				// Payload and length
				memory_to_send, bytes_to_send,
				// Optional key and its length
				NULL, 0,
				// Message opaque, provided in delivery report callback as msg_opaque
				NULL) == -1)
			{
				fprintf(stderr, "%% Failed to produce message: %s\n", rd_kafka_err2str(rd_kafka_errno2err(errno)));
				total_bytes_sent -= bytes_to_send;
				// Poll to handle delivery reports
				rd_kafka_poll(rk, 0);
				continue;
			}

			total_bytes_sent += bytes_to_send;
			total_msgs_sent ++;

			// Calculate throughput
			gettimeofday(&timer_end, NULL);

			if(sleep_time > 0)
				usleep(sleep_time);

			double delta_us = (timer_end.tv_sec * ONE_MILLION + timer_end.tv_usec) - (timer_start.tv_sec * ONE_MILLION + timer_start.tv_usec);
			double delta_s = delta_us/(double)ONE_MILLION;

			if(fabs(delta_s - 1) < throughput_tol)
			{
				double mbps = (8*total_bytes_sent)/(delta_us);
				// this is because of the granularity
				if (mbps == 0)
					printf("No bytes sent in %.6f secs\n", delta_s);
				else
					printf("Sent %ld B in %.2f s (%.4f Mbps). total msgs: %ld\n", total_bytes_sent, delta_s, mbps, total_msgs_sent);
				total_bytes_sent = 0;
				gettimeofday(&timer_start, NULL);
			}

			// Poll to handle delivery reports/
			rd_kafka_poll(rk, 0);

			line_offset = offset_table[lines_sent + lines_to_send -1];

			if(is_msgs_limited && total_msgs_sent == msgs_to_send)
			{
				send_more_msgs = false;
				break;
			}
		}
	}
	cleanup(rk, rkt, total_msgs_sent);
	return 0;
}
