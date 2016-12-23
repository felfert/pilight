/*
	Copyright (C) 2013 - 2016 CurlyMo

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#ifdef _WIN32
	#include <conio.h>
	#include <pthread.h>
#endif

#include "libs/libuv/uv.h"
#include "libs/pilight/core/threadpool.h"
#include "libs/pilight/core/timerpool.h"
#include "libs/pilight/core/eventpool.h"
#include "libs/pilight/core/pilight.h"
#include "libs/pilight/core/common.h"
#include "libs/pilight/core/gc.h"
#include "libs/pilight/core/log.h"
#include "libs/pilight/core/options.h"
#include "libs/pilight/core/socket.h"
#include "libs/pilight/core/json.h"
#include "libs/pilight/core/ssdp.h"
#include "libs/pilight/core/dso.h"

#include "libs/pilight/protocols/protocol.h"

#define CONNECT			1
#define VALIDATE		2
#define SEND				3
#define SUCCESS			4

static uv_tty_t *tty_req = NULL;
static uv_signal_t *signal_req = NULL;

typedef struct data_t {
	int steps;
	char *buffer;
	size_t buflen;
} data_t;

typedef struct pname_t {
	char *name;
	char *desc;
	struct pname_t *next;
} pname_t;

typedef struct ssdp_list_t {
	char server[INET_ADDRSTRLEN+1];
	unsigned short port;
	char name[17];
	struct ssdp_list_t *next;
} ssdp_list_t;

static struct pname_t *pname = NULL;

static struct ssdp_list_t *ssdp_list = NULL;
static int ssdp_list_size = 0;
static unsigned short connected = 0;
static unsigned short connecting = 0;
static unsigned short found = 0;
static char *instance = NULL;
static struct JsonNode *code = NULL;
static char *uuid = NULL;

static void signal_cb(uv_signal_t *, int);
static void on_write(uv_write_t *req, int status);

static int main_gc(void) {
	if(code != NULL) {
		json_delete(code);
		code = NULL;
	}
	log_shell_disable();

	struct ssdp_list_t *tmp = NULL;
	while(ssdp_list) {
		tmp = ssdp_list;
		ssdp_list = ssdp_list->next;
		FREE(tmp);
	}

	if(instance != NULL) {
		FREE(instance);
	}
	if(uuid != NULL) {
		FREE(uuid);
	}

	protocol_gc();
	timer_thread_gc();
	eventpool_gc();

	log_shell_disable();
	log_gc();

	if(progname != NULL) {
		FREE(progname);
		progname = NULL;
	}

	return 0;
}

static void timeout_cb(uv_timer_t *param) {
	if(connected == 0) {
		logprintf(LOG_ERR, "could not connect to the pilight instance");
		signal_cb(NULL, SIGINT);
	}
}

static void ssdp_not_found(uv_timer_t *param) {
	if(found == 0) {
		logprintf(LOG_ERR, "could not find pilight instance: %s", instance);
		signal_cb(NULL, SIGINT);
	}
}

static void alloc_cb(uv_handle_t *handle, size_t len, uv_buf_t *buf) {
	buf->len = len;
	if((buf->base = malloc(len)) == NULL) {
		OUT_OF_MEMORY
	}
	memset(buf->base, 0, len);
}

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *rbuf) {
  if(nread == -1) {
    logprintf(LOG_ERR, "socket read failed");
    return;
  }
	struct data_t *data = stream->data;
	switch(data->steps) {
		case VALIDATE: {
			if(strncmp(rbuf->base, "{\"status\":\"success\"}", 20) == 0) {
				data->steps = SUCCESS;

				struct JsonNode *json = json_mkobject();
				json_append_member(json, "action", json_mkstring("send"));
				if(uuid != NULL) {
					json_append_member(code, "uuid", json_mkstring(uuid));
				}
				json_append_member(json, "code", code);

				uv_write_t *write_req = MALLOC(sizeof(uv_write_t));
				write_req->data = data;
				char out[BUFFER_SIZE];
				uv_buf_t wbuf = uv_buf_init(out, BUFFER_SIZE);

				wbuf.base = json_stringify(json, NULL);
				wbuf.len = strlen(wbuf.base);
				
				uv_write(write_req, stream, &wbuf, 1, on_write);
				json_delete(json);
				json_free(wbuf.base);
				code = NULL;
			}
			free(rbuf->base);
		} break;
		case SUCCESS: {
			if(strncmp(rbuf->base, "{\"status\":\"success\"}", 20) != 0) {
				logprintf(LOG_ERR, "failed to send codes");
			}
			signal_cb(NULL, SIGINT);
			free(rbuf->base);
			FREE(data);
		} break;
	}
}


static void on_write(uv_write_t *req, int status) {
	struct data_t *data = req->data;
	req->handle->data = data;
	uv_read_start(req->handle, alloc_cb, on_read);
	FREE(req);
}

static void on_connect(uv_connect_t *req, int status) {
  if(status != 0) {
    logprintf(LOG_ERR, "socket connect failed");
    return;
	}

	uv_write_t *write_req = MALLOC(sizeof(uv_write_t));
	char out[BUFFER_SIZE];
	uv_buf_t buf = uv_buf_init(out, BUFFER_SIZE);
	
	connected = 1;

	buf.len = sprintf(buf.base, "{\"action\":\"identify\"}");	

	struct data_t *data = NULL;
	if((data = MALLOC(sizeof(struct data_t))) == NULL) {
		OUT_OF_MEMORY
	}
	data->steps = VALIDATE;
	data->buffer = NULL;
	data->buflen = 0;
	write_req->data = data;
	uv_write(write_req, req->handle, &buf, 1, on_write);

	FREE(req);
}

static void connect_to_server(char *server, int port) {
	struct sockaddr_in addr;
	uv_tcp_t *client_req = NULL;
	uv_connect_t *connect_req = NULL;	
	uv_timer_t *socket_timeout_req = NULL;
	
	if((client_req = MALLOC(sizeof(uv_tcp_t))) == NULL) {
		OUT_OF_MEMORY
	}

	if((connect_req = MALLOC(sizeof(uv_connect_t))) == NULL) {
		OUT_OF_MEMORY
	}

	if((socket_timeout_req = MALLOC(sizeof(uv_timer_t))) == NULL) {
		OUT_OF_MEMORY
	}

	int steps = CONNECT;
	connect_req->data = &steps;

	uv_tcp_init(uv_default_loop(), client_req);
	uv_ip4_addr(server, port, &addr);
	uv_tcp_connect(connect_req, client_req, (const struct sockaddr *)&addr, on_connect);

	uv_timer_init(uv_default_loop(), socket_timeout_req);
	uv_timer_start(socket_timeout_req, timeout_cb, 1000, 0);	
}

static int select_server(int server) {
	struct ssdp_list_t *tmp = ssdp_list;
	int i = 0;
	while(tmp) {
		if((ssdp_list_size-i) == server) {
			connect_to_server(tmp->server, tmp->port);
			return 0;
		}
		i++;
		tmp = tmp->next;
	}
	return -1;
}
static void *ssdp_found(int reason, void *param) {
	struct reason_ssdp_received_t *data = param;
	struct ssdp_list_t *node = NULL;
	int match = 0;

	if(connecting == 0 && data->ip != NULL && data->port > 0 && data->name != NULL) {
		if(instance == NULL) {
			struct ssdp_list_t *tmp = ssdp_list;
			while(tmp) {
				if(strcmp(tmp->server, data->ip) == 0 && tmp->port == data->port) {
					match = 1;
					break;
				}
				tmp = tmp->next;
			}
			if(match == 0) {
				if((node = MALLOC(sizeof(struct ssdp_list_t))) == NULL) {
					OUT_OF_MEMORY
				}
				strncpy(node->server, data->ip, INET_ADDRSTRLEN);
				node->port = data->port;
				strncpy(node->name, data->name, 17);

				ssdp_list_size++;

				printf("\r[%2d] %15s:%-5d %-16s\n", ssdp_list_size, node->server, node->port, node->name);
				printf("To which server do you want to connect?: ");
				fflush(stdout);

				node->next = ssdp_list;
				ssdp_list = node;
			}
		} else {
			if(strcmp(data->name, instance) == 0) {
				found = 1;
				connect_to_server(data->ip, data->port);
			}
		}
	}
	return NULL;
}

static void read_cb(uv_stream_t *stream, ssize_t len, const uv_buf_t *buf) {
	buf->base[len-1] = '\0';

#ifdef _WIN32
	/* Remove windows vertical tab */
	if(buf->base[len-2] == 13) {
		buf->base[len-2] = '\0';
	}
#endif

	if(isNumeric(buf->base) == 0) {
		select_server(atoi(buf->base));
	}
	free(buf->base);
}

void signal_cb(uv_signal_t *handle, int signum) {
	if(instance == NULL && tty_req != NULL) {
		uv_read_stop((uv_stream_t *)tty_req);
		tty_req = NULL;
	}
	uv_stop(uv_default_loop());
	main_gc();
}

void close_cb(uv_handle_t *handle) {
	FREE(handle);
}

static void walk_cb(uv_handle_t *handle, void *arg) {
	if(!uv_is_closing(handle)) {
		uv_close(handle, close_cb);
	}
}

static void sort_list(void) {
	struct pname_t *a = NULL;
	struct pname_t *b = NULL;
	struct pname_t *c = NULL;
	struct pname_t *e = NULL;
	struct pname_t *tmp = NULL;

	while(pname && e != pname->next) {
		c = a = pname;
		b = a->next;
		while(a != e) {
			if(strcmp(a->name, b->name) > 0) {
				if(a == pname) {
					tmp = b->next;
					b->next = a;
					a->next = tmp;
					pname = b;
					c = b;
				} else {
					tmp = b->next;
					b->next = a;
					a->next = tmp;
					c->next = b;
					c = b;
				}
			} else {
				c = a;
				a = a->next;
			}
			b = a->next;
			if(b == e)
				e = a;
		}
	}
}

static void main_loop(int onclose) {
	if(onclose == 1) {
		signal_cb(NULL, SIGINT);
	}
	uv_run(uv_default_loop(), UV_RUN_DEFAULT);	
	uv_walk(uv_default_loop(), walk_cb, NULL);
	uv_run(uv_default_loop(), UV_RUN_ONCE);

	if(onclose == 1) {
		while(uv_loop_close(uv_default_loop()) == UV_EBUSY) {
			usleep(10);
		}
	}
}

int main(int argc, char **argv) {
	uv_replace_allocator(_MALLOC, _REALLOC, _CALLOC, _FREE);

	log_init();
	log_file_disable();
	log_shell_enable();
	log_level_set(LOG_NOTICE);
	
	pilight.process = PROCESS_CLIENT;

	if((progname = MALLOC(13)) == NULL) {
		OUT_OF_MEMORY
	}
	strcpy(progname, "pilight-send");

	if((signal_req = MALLOC(sizeof(uv_signal_t))) == NULL) {
		OUT_OF_MEMORY
	}

	uv_signal_init(uv_default_loop(), signal_req);
	uv_signal_start(signal_req, signal_cb, SIGINT);	

	struct options_t *options = NULL;

	int raw[MAXPULSESTREAMLENGTH-1];
	char *args = NULL, *recvBuff = NULL;
	char *protobuffer = NULL;
	int match = 0;

	int help = 0;
	int version = 0;
	int protohelp = 0;

	char *server = NULL;
	unsigned short port = 0;

	struct protocol_t *protocol = NULL;

	options_add(&options, 'H', "help", OPTION_NO_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'V', "version", OPTION_NO_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'p', "protocol", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'S', "server", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, "^(([0-9]|[1-9][0-9]|1[0-9]{2}|2[0-4][0-9]|25[0-5]).){3}([0-9]|[1-9][0-9]|1[0-9]{2}|2[0-4][0-9]|25[0-5])$");
	options_add(&options, 'P', "port", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, "[0-9]{1,4}");
	options_add(&options, 'I', "instance", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'U', "uuid", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, "[a-zA-Z0-9]{4}-[a-zA-Z0-9]{2}-[a-zA-Z0-9]{2}-[a-zA-Z0-9]{2}-[a-zA-Z0-9]{6}");

	/* Get the protocol to be used */
	while(1) {
		int c;
		c = options_parse(&options, argc, argv, 0, &args);
		if(c == -1)
			break;
		if(c == -2)
			c = 'H';
		switch(c) {
			case 'p':
				if(strlen(args) == 0) {
					logprintf(LOG_INFO, "options '-p' and '--protocol' require an argument");
					goto close;
				} else {
					if((protobuffer = REALLOC(protobuffer, strlen(args)+1)) == NULL) {
						OUT_OF_MEMORY
					}
					strcpy(protobuffer, args);
				}
			break;
			case 'V':
				version = 1;
			break;
			case 'H':
				help = 1;
			break;
			case 'I':
				if((instance = MALLOC(strlen(args)+1)) == NULL) {
					OUT_OF_MEMORY
				}
				strcpy(instance, args);
			break;
			case 'S':
				if((server = REALLOC(server, strlen(args)+1)) == NULL) {
					OUT_OF_MEMORY
				}
				strcpy(server, args);
			break;
			case 'P':
				port = (unsigned short)atoi(args);
			break;
			case 'U':
				if((uuid = REALLOC(uuid, strlen(args)+1)) == NULL) {
					OUT_OF_MEMORY
				}
				strcpy(uuid, args);
			break;
			default:;
		}
	}

	/* Initialize protocols */
	protocol_init();

	/* Check if a protocol was given */
	if(protobuffer != NULL && strlen(protobuffer) > 0 && strcmp(protobuffer, "-V") != 0) {
		if(strlen(protobuffer) > 0 && version) {
			printf("-p and -V cannot be combined\n");
		} else {
			struct protocols_t *pnode = protocols;
			/* Retrieve the used protocol */
			while(pnode) {
				/* Check if the protocol exists */
				protocol = pnode->listener;
				if(protocol_device_exists(protocol, protobuffer) == 0 && match == 0 && protocol->createCode != NULL) {
					match=1;
					/* Check if the protocol requires specific CLI arguments
					   and merge them with the main CLI arguments */
					if(protocol->options && help == 0) {
						options_merge(&options, &protocol->options);
					} else if(help == 1) {
						protohelp=1;
					}
					break;
				}
				pnode = pnode->next;
			}
			/* If no protocols matches the requested protocol */
			if(match == 0) {
				logprintf(LOG_ERR, "this protocol is not supported or does not support sending");
				goto close;
			}
		}
	}

	/* Store all CLI arguments for later usage
	   and also check if the CLI arguments where
	   used correctly by the user. This will also
	   fill all necessary values in the options struct */
	while(1) {
		int c;
		c = options_parse(&options, argc, argv, 2, &args);

		if(c == -1)
			break;
		if(c == -2) {
			if(match == 1) {
				protohelp = 1;
			} else {
				help = 1;
			}
			break;
		}
	}

	/* Display help or version information */
	if(version == 1) {
		printf("%s v%s\n", progname, PILIGHT_VERSION);
		goto close;
	} else if(help == 1 || protohelp == 1 || match == 0) {
		if(protohelp == 1 && match == 1 && protocol->printHelp)
			printf("Usage: %s -p %s [options]\n", progname, protobuffer);
		else
			printf("Usage: %s -p protocol [options]\n", progname);
		if(help == 1) {
			printf("\t -H --help\t\t\tdisplay this message\n");
			printf("\t -V --version\t\t\tdisplay version\n");
			printf("\t -S --server=x.x.x.x\t\tconnect to server address\n");
			printf("\t -P --port=xxxx\t\t\tconnect to server port\n");
			printf("\t -I --instance=name\t\tconnect to pilight instance\n");
			printf("\t -C --config\t\t\tconfig file\n");
			printf("\t -U --uuid=xxx-xx-xx-xx-xxxxxx\tUUID\n");
			printf("\t -p --protocol=protocol\t\tthe protocol that you want to control\n");
		}
		if(protohelp == 1 && match == 1 && protocol->printHelp) {
			printf("\n\t[%s]\n", protobuffer);
			protocol->printHelp();
		} else {
			printf("\nThe supported protocols are:\n");
			struct protocols_t *pnode = protocols;
			/* Retrieve the used protocol */
			while(pnode) {
				protocol = pnode->listener;
				if(protocol->createCode) {
					struct protocol_devices_t *tmpdev = protocol->devices;
					while(tmpdev) {
						struct pname_t *node = MALLOC(sizeof(struct pname_t));
						if(node == NULL) {
							OUT_OF_MEMORY
						}
						if((node->name = MALLOC(strlen(tmpdev->id)+1)) == NULL) {
							OUT_OF_MEMORY
						}
						strcpy(node->name, tmpdev->id);
						if((node->desc = MALLOC(strlen(tmpdev->desc)+1)) == NULL) {
							OUT_OF_MEMORY
						}
						strcpy(node->desc, tmpdev->desc);
						node->next = pname;
						pname = node;
						tmpdev = tmpdev->next;
					}
				}
				pnode = pnode->next;
			}
			sort_list();
			struct pname_t *ptmp = NULL;
			while(pname) {
				ptmp = pname;
				printf("\t %s\t\t",ptmp->name);
				if(strlen(ptmp->name) < 7)
					printf("\t");
				if(strlen(ptmp->name) < 15)
					printf("\t");
				printf("%s\n", ptmp->desc);
				FREE(ptmp->name);
				FREE(ptmp->desc);
				pname = pname->next;
				FREE(ptmp);
			}
			FREE(pname);
		}
		goto close;
	}

	code = json_mkobject();
	int itmp = 0;
	/* Check if we got sufficient arguments from this protocol */
	struct options_t *tmp = options;
	while(tmp) {
		if(strlen(tmp->name) > 0) {
			/* Only send the CLI arguments that belong to this protocol, the protocol name
			and those that are called by the user */
			if((options_get_id(&protocol->options, tmp->name, &itmp) == 0)
			    && tmp->vartype == JSON_STRING && tmp->string_ != NULL
				&& (strlen(tmp->string_) > 0)) {
				if(isNumeric(tmp->string_) == 0) {
					json_append_member(code, tmp->name, json_mknumber(atof(tmp->string_), nrDecimals(tmp->string_)));
				} else {
					json_append_member(code, tmp->name, json_mkstring(tmp->string_));
				}
			}
			if(strcmp(tmp->name, "protocol") == 0 && strlen(tmp->string_) > 0) {
				JsonNode *jprotocol = json_mkarray();
				json_append_element(jprotocol, json_mkstring(tmp->string_));
				json_append_member(code, "protocol", jprotocol);
			}
		}
		tmp = tmp->next;
	}

	eventpool_init(EVENTPOOL_NO_THREADS);
	eventpool_callback(REASON_SSDP_RECEIVED, ssdp_found);

	memset(raw, 0, MAXPULSESTREAMLENGTH-1);
	protocol->raw = raw;
	char message[255];
	if(protocol->createCode(code, message) == 0) {
		if(server != NULL && port > 0) {
			connect_to_server(server, port);
		} else {
			ssdp_seek();
			uv_timer_t *ssdp_reseek_req = NULL;
			if((ssdp_reseek_req = MALLOC(sizeof(uv_timer_t))) == NULL) {
				OUT_OF_MEMORY
			}

			uv_timer_init(uv_default_loop(), ssdp_reseek_req);
			uv_timer_start(ssdp_reseek_req, (void (*)(uv_timer_t *))ssdp_seek, 3000, 3000);
			if(instance == NULL) {
				printf("[%2s] %15s:%-5s %-16s\n", "#", "server", "port", "name");
				printf("To which server do you want to connect?:\r");
				fflush(stdout);

				if((tty_req = MALLOC(sizeof(uv_tty_t))) == NULL) {
					OUT_OF_MEMORY
				}
				
				uv_tty_init(uv_default_loop(), tty_req, 0, 1);
				uv_read_start((uv_stream_t *)tty_req, alloc_cb, read_cb);
			} else {
				uv_timer_t *ssdp_not_found_req = NULL;
				if((ssdp_not_found_req = MALLOC(sizeof(uv_timer_t))) == NULL) {
					OUT_OF_MEMORY
				}

				uv_timer_init(uv_default_loop(), ssdp_not_found_req);
				uv_timer_start(ssdp_not_found_req, ssdp_not_found, 1000, 0);
			}
		}


		main_loop(0);

		FREE(progname);
	}

close:
	options_delete(options);
	options_gc();

	main_loop(1);

	if(recvBuff != NULL) {
		FREE(recvBuff);
	}
	if(server != NULL) {
		FREE(server);
	}
	if(protobuffer != NULL) {
		FREE(protobuffer);
	}

	return EXIT_SUCCESS;
}
