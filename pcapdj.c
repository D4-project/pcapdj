/*
* pcapdj - dispatch pcap files
*
* Copyright (C) 2013 Gerard Wagener
* Copyright (C) 2013 CIRCL Computer Incident Response Center Luxembourg 
* (SMILE gie).
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU Affero General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU Affero General Public License for more details.
*
* You should have received a copy of the GNU Affero General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <getopt.h>
#include <assert.h>
#include <string.h>
#include <wtap.h>
#include <unistd.h>
#include <signal.h>
#include <wsutil/buffer.h>
#define PQUEUE "PCAPDJ_IN_QUEUE"
#define RQUEUE "PCAPDJ_PROCESSED"
#define NEXTJOB "PCAPDJ_NEXT"
#define AKEY "PCAPDJ_AUTH"
#define DEFAULT_SRV "127.0.0.1"
#define POLLINT 100000
#define PCAPDJ_STATE "PCAPDJ_STATE"
#define PCAPDJ_STATE_DONE "DONE"

/* Internal pcapdj states */
#define PCAPDJ_I_STATE_RUN 0
#define PCAPDJ_I_STATE_SUSPEND 1
#define PCAPDJ_I_STATE_AUTH_WAIT 2
#define PCAPDJ_I_STATE_FEED 3 
#include <hiredis/hiredis.h>

/* FIXME No atomicity is assured so it might be that they are not accurate */
typedef struct statistics_s {
    u_int64_t num_files;
    u_int64_t num_packets;
    u_int64_t sum_cap_lengths;
    u_int64_t sum_lengths;
    u_int64_t num_suspend;
    u_int8_t state;
    u_int8_t oldstate;
    time_t startepoch;
    struct tm *starttime;
} statistics_t;

/* Global variables */
sig_atomic_t sigusr1_suspend = 0;
statistics_t stats;

void usage(void)
{
    
    printf("pcapdj [-h] -b namedpipe [-s redis_server] -p [redis_srv_port] [-q redis_queue]\n\n");
    printf("Connects to the redis instance specified with by the redis_server\n");
    printf("and redis_srv_port.\n\n"); 

    printf("Read a list of pcap-ng files from the queue PCAPDJ_IN_QUEUE by default\n");
    printf("or the queue specified with the -q flag is set.\n");
    printf("Open the pcap-ng file and feed each packet to the fifo buffer\n"); 
    printf("specified by with the -b option.  When a pcap file from the list\n"); 
    printf("has been transferred to the buffer update the queue PCAPDJ_PROCESSED\n");
    printf("with the filename that just was processed.\n\n"); 

    printf("Update the  PCAPDJ_NEXT with the next file that is beeing processed.\n");
    printf("Poll PCAPDJ_AUTH key. When the value of this key corresponds to the next file then use \n");
    printf("the next pcap file and feed the fifo buffer with the packets.\n");
    printf("\nWhen the last packet of the last file has been processed the fifo\n");
    printf("the file handle  is closed.\n"); 
}

void suspend_pcapdj_if_needed(const char *state) 
{
    if (sigusr1_suspend) {
        fprintf(stderr,"[INFO] pcapdj is suspended. %s\n",state);
        while (sigusr1_suspend) { 
                usleep(POLLINT);
        }
    }
}

void display_stats()
{
    char stimebuf[64];
    u_int64_t uptime;
    time_t t;
    /* Display accounting numbers */
    if (strftime((char*)&stimebuf, 64, "%Y-%d-%m %H:%M:%S",stats.starttime))
        printf("[STATS] Start time:%s\n",stimebuf);
    t = time(NULL);
    uptime = t - stats.startepoch;
    printf("[STATS] Uptime:%ld (seconds)\n", uptime);

    /* Describe the internal state */
    switch (stats.state) {
        case PCAPDJ_I_STATE_RUN:
            printf("[STATS] Internal state:Running\n");
            break;
        case PCAPDJ_I_STATE_SUSPEND:
            printf("[STATS] Internal state:Suspended\n");
            break;
        case PCAPDJ_I_STATE_AUTH_WAIT:
            printf("[STATS] Internal state:Waiting for authorization\n");
            break;
        case PCAPDJ_I_STATE_FEED:
            printf("[STATS] Internal state:Feeding fifo buffer\n");
            break;
        default:
            printf("[STATS] Internal state:Unknown\n");        
    }
    printf("[STATS] Number of suspensions:%ld\n",stats.num_suspend);
    printf("[STATS] Number of files:%ld\n",stats.num_files);
    printf("[STATS] Number of packets:%ld\n",stats.num_packets);
    printf("[STATS] Number of cap_lengths:%ld\n",stats.sum_cap_lengths);
    printf("[STATS] Number of lengths:%ld\n",stats.sum_lengths);
}

void sig_handler(int signal_number)
{
    if (signal_number == SIGUSR1) {
        sigusr1_suspend=~sigusr1_suspend;

        if (sigusr1_suspend) {
            printf("[INFO] Suspending pcapdj\n");
            stats.oldstate = stats.state;
            stats.state = PCAPDJ_I_STATE_SUSPEND;
            stats.num_suspend++;
            /* This function should not block otherwise the resume does not work */
        }else{
            printf("[INFO] Resuming pcapdj\n");
            stats.state = stats.oldstate;
            stats.oldstate = PCAPDJ_I_STATE_SUSPEND;
        }
    }
    if (signal_number == SIGUSR2) {
        display_stats();
    }
}

void update_processed_queue(redisContext* ctx, char *filename)
{
    /* FIXME errors are currently ignored */
    redisReply *reply;
    reply = redisCommand(ctx,"RPUSH %s %s",RQUEUE, filename);
    if (reply)
        freeReplyObject(reply);
}
void update_next_file(redisContext* ctx, char* filename)
{
    /* FIXME Currently we don't care if the field was set */
    redisReply *reply;
    reply = redisCommand(ctx,"RPUSH %s %s", NEXTJOB, filename);
    if (reply)
        freeReplyObject(reply);
} 

void delete_next_file_queue(redisContext* ctx)
{
    /* FIXME errors are ignored */
    redisReply * reply;
    reply = redisCommand(ctx, "DEL %s",NEXTJOB);
    if (reply)
        freeReplyObject(reply);
}


void delete_auth_file(redisContext* ctx, char* filename)
{
    /* FIXME errors are ignored */
    redisReply * reply;
    reply = redisCommand(ctx, "SREM %s %s", AKEY, filename);
    if (reply)
        freeReplyObject(reply);
}

void wait_auth_to_proceed(redisContext* ctx, char* filename)
{
    redisReply *reply;
    stats.state = PCAPDJ_I_STATE_AUTH_WAIT;
    /* If there is an error the program waits forever */
    
    do {
        reply = redisCommand(ctx,"SISMEMBER %s %s",AKEY, filename);
        if (reply){
            if (reply->type == REDIS_REPLY_INTEGER) {
                /* Delete the filename from the set if found */
                if (reply->integer == 1){
                    delete_auth_file(ctx, filename);
                    fprintf(stderr, "[INFO] Got authorization to process %s\n",filename);
                    freeReplyObject(reply);
                    return;
                }
            }       
            freeReplyObject(reply);
        }else{
            fprintf(stderr,"[ERROR] redis server did not replied for the authorization\n");
        }
        usleep(POLLINT);
    } while (1);
}

void process_file(redisContext* ctx, wtap_dumper* dumper, char* filename)
{
    wtap *wth;
    wtap_rec rec;
    int err, write_err;
    char *errinfo;
    gchar *write_err_info;
    gint64 data_offset;
    Buffer readbuf;

    wtap_rec_init(&rec);
    ws_buffer_init(&readbuf, 1500);

    fprintf(stderr,"[INFO] Next file to process %s\n",filename);
    update_next_file(ctx, filename);
    fprintf(stderr,"[INFO] Waiting authorization to process file %s\n",filename);
    wait_auth_to_proceed(ctx, filename);
    wth = wtap_open_offline ( filename, WTAP_TYPE_AUTO, (int*)&err,
                             (char**)&errinfo, FALSE);
    if (wth) {
        stats.num_files++;
        /* Loop over the packets and adjust the headers */
        while (wtap_read(wth, &rec, &readbuf, &err, &errinfo, &data_offset)) {
            suspend_pcapdj_if_needed("Stop feeding buffer.");
            stats.state = PCAPDJ_I_STATE_FEED;
            /* Need to convert micro to nano seconds */
            wtap_dump(dumper, &rec, ws_buffer_start_ptr(&readbuf), &write_err, &write_err_info);
            stats.num_packets++;
            stats.sum_cap_lengths+=rec.rec_header.packet_header.caplen;
            stats.sum_lengths+=rec.rec_header.packet_header.caplen;
        }
        update_processed_queue(ctx, filename);
        wtap_close(wth);
        wtap_rec_cleanup(&rec);
        ws_buffer_free(&readbuf);
    }else{
        fprintf(stderr, "[ERROR] Could not open filename %s,cause=%s\n",filename,
                wtap_strerror(err));
    }
}

int process_input_queue(wtap_dumper *dumper, char* redis_server, int redis_srv_port, char* redis_queue)
{
    redisContext* ctx; 
    redisReply* reply;    
    int rtype;
    ctx = redisConnect(redis_server, redis_srv_port);

    if (ctx != NULL && ctx->err) {
        fprintf(stderr,"[ERROR] Could not connect to redis. %s.\n", ctx->errstr);
        return EXIT_FAILURE;
    }

    do {
        reply = redisCommand(ctx,"LPOP %s", redis_queue); 
        if (!reply){
            fprintf(stderr,"[ERROR] Redis error %s\n",ctx->errstr);
            return EXIT_FAILURE;
        }
        /* We got a reply */
        rtype = reply->type;
        if (rtype == REDIS_REPLY_STRING) {
            process_file(ctx, dumper, reply->str);
            
        }
        freeReplyObject(reply);
    } while (rtype != REDIS_REPLY_NIL);
    /* Notify other party that everything is done */
    reply = redisCommand(ctx, "SET %s %s",PCAPDJ_STATE, PCAPDJ_STATE_DONE);
    if (reply) {
        freeReplyObject(reply);
    }
    /* Do the cleanup */
    delete_next_file_queue(ctx);
    redisFree(ctx);
    return EXIT_SUCCESS;
}
 

void init(void)
{
    struct sigaction sa;
    
    sigusr1_suspend = 0;
    memset(&sa,0,sizeof(sa));
    memset(&stats,0,sizeof(statistics_t));
    
    /* Update the start time */
    stats.startepoch = time(NULL);
    stats.starttime = localtime(&stats.startepoch);
    assert(stats.starttime);

    wtap_init(FALSE);

    /* Install signal handler */
    sa.sa_handler = &sig_handler;
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
}

int main(int argc, char* argv[])
{

    int opt, r, redis_srv_port, write_err;
    char* redis_server;
    char* namedpipe;
    char* redis_queue;
    FILE *fifo;
    wtap_dumper *pdh = NULL;
    wtap_dump_params params = WTAP_DUMP_PARAMS_INIT;

    init();
    
    namedpipe = calloc(128,1);
    assert(namedpipe);  

    redis_queue = calloc(128,1);
    assert(redis_queue);  

    redis_server = calloc(64,1);
    assert(redis_server);

    redis_srv_port = 6379;        
    while ((opt = getopt(argc, argv, "b:hs:p:q:")) != -1) {
        switch (opt) {
            case 's':
                strncpy(redis_server, optarg, 64);
                break;
            case 'p':
                redis_srv_port = atoi(optarg);
                break;
            case 'b':
                strncpy(namedpipe , optarg, 128);
                break;
            case 'q':
                strncpy(redis_queue , optarg, 128);
                break;
            case 'h':
                usage();
                return EXIT_SUCCESS;
            default: /* '?' */
                fprintf(stderr, "[ERROR] Invalid command line was specified\n");
        }
    }
    /* Set default values if needed */
    if (!redis_server[0])
        strncpy(redis_server,DEFAULT_SRV,64);
    /* Connect to redis */
    if (!namedpipe[0]){
        fprintf(stderr,"[ERROR] A named pipe must be specified\n");
        return EXIT_FAILURE; 
    }
    if (!redis_queue[0])
        strncpy(redis_queue,PQUEUE,128);

    fifo = fopen(namedpipe, "wb");
    if (fifo == NULL) {
        fprintf(stderr, "[ERROR]: %d (%s)\n", errno, strerror(errno));
    }

    fprintf(stderr, "[INFO] redis_server = %s\n",redis_server);
    fprintf(stderr, "[INFO] redis_port = %d\n",redis_srv_port);
    fprintf(stderr, "[INFO] redis_queue = %s\n", redis_queue);
    fprintf(stderr, "[INFO] named pipe = %s\n", namedpipe);
    fprintf(stderr, "[INFO] pid = %d\n",(int)getpid());
    
    params.encap =  WTAP_ENCAP_ETHERNET;
    pdh = wtap_dump_fdopen(fileno(fifo), WTAP_FILE_TYPE_SUBTYPE_PCAPNG, WTAP_UNCOMPRESSED, &params, &write_err);
    if (pdh != NULL){
        r = process_input_queue(pdh, redis_server, redis_srv_port, redis_queue);
        if (r == EXIT_FAILURE) {
            fprintf(stderr,"[ERROR] Something went wrong in during processing");
        }else{
            fprintf(stderr,"[INFO] All went fine. No files in the pipe to process.\n");
        }
        wtap_dump_close(pdh, &write_err);
        wtap_dump_params_cleanup(&params);
        return r;
    }else{
        fprintf(stderr, "[ERROR]: wtap_dump_fdopen %d (%s)\n", write_err, strerror(write_err));
    }
    return EXIT_FAILURE;
    wtap_dump_close(pdh, &write_err);
    wtap_dump_params_cleanup(&params);
}