#include <stdio.h>
#include <sys/socket.h>
#include <string.h>
#include <signal.h>
#include "ini/ini.h"
#include "tilelite.h"
#include "tile_renderer.h"
#include "tcp.h"
#include "tl_time.h"
#include "msgpack.h"

#ifdef TILELITE_EPOLL
#include "ev_loop_epoll.h"
#elif TILELITE_KQUEUE
#include "ev_loop_kqueue.h"
#endif

void set_signal_handler(int sig_num, void (*handler)(int sig_num)) {
  struct sigaction action;

  memset(&action, 0, sizeof(action));

  sigemptyset(&action.sa_mask);
  action.sa_handler = handler;
  sigaction(sig_num, &action, NULL);
}

int ini_parse_callback(void* user, const char* section, const char* name,
                       const char* value) {
  tilelite_config* conf = (tilelite_config*)user;

  if (strcmp(name, "plugins") == 0) {
    register_plugins(value);
  } else if (strcmp(name, "fonts") == 0) {
    register_fonts(value);
  } else {
    (*conf)[name] = value;
  }

  return 1;
}

void set_defaults(tilelite_config* conf) {
  auto set_key = [conf](const char* key, const char* value) {
    if (conf->count(key) == 0) (*conf)[key] = value;
  };

  set_key("threads", "1");
  set_key("tile_db", "tiles.db");
  set_key("port", "9567");
}

enum message_type : uint64_t {
  mt_invalid = 0,
  mt_tile_request = 1,
  mt_prerender = 2
};

bool read_request(const char* data, int len, tile* tile_req) {
  msgpack_zone mempool;
  msgpack_zone_init(&mempool, 2048);

  msgpack_object request;

  if (msgpack_unpack(data, len, NULL, &mempool, &request) !=
      MSGPACK_UNPACK_SUCCESS) {
    printf("failed to unpack request\n");
    msgpack_zone_destroy(&mempool);
    return false;
  }

  if (request.type != MSGPACK_OBJECT_MAP) {
    printf("invalid request type\n");
    msgpack_zone_destroy(&mempool);
    return false;
  }

  size_t num_elements = request.via.map.size;

  if (num_elements < 2) {
    printf("invalid key count\n");
    msgpack_zone_destroy(&mempool);
    return false;
  }

  message_type mtype = mt_invalid;
  msgpack_object_map content = { 0 };
  for (size_t i = 0; i < num_elements; i++) {
    msgpack_object_kv* kv = &request.via.map.ptr[i];
    if (kv->key.type != MSGPACK_OBJECT_STR) continue;

    msgpack_object_str key = kv->key.via.str; 
    if (strncmp("type", key.ptr, key.size) == 0) {
      mtype = message_type(kv->val.via.u64);
    } else if (strncmp("content", key.ptr, key.size) == 0) {
      content = kv->val.via.map;
    }
  }

  if (mtype == mt_invalid || content.size == 0) {
    printf("invalid type/content\n");
    msgpack_zone_destroy(&mempool);
    return false;
  } else if (mtype == mt_tile_request) {
    for (size_t i = 0; i < content.size; i++) {
      msgpack_object_kv kv = content.ptr[i];
      if (kv.key.type != MSGPACK_OBJECT_STR) continue;

      msgpack_object_str key = kv.key.via.str; 
      if (strncmp("x", key.ptr, key.size) == 0) {
        tile_req->x = int(kv.val.via.i64);
      } else if (strncmp("y", key.ptr, key.size) == 0) {
        tile_req->y = int(kv.val.via.i64);
      } else if (strncmp("z", key.ptr, key.size) == 0) {
        tile_req->z = int(kv.val.via.i64);
      } else if (strncmp("w", key.ptr, key.size) == 0) {
        tile_req->w = int(kv.val.via.i64);
      } else if (strncmp("h", key.ptr, key.size) == 0) {
        tile_req->h = int(kv.val.via.i64);
      }
    }
  } else {
    msgpack_zone_destroy(&mempool);
    return false;
  }

  msgpack_zone_destroy(&mempool);

  return true;
}

int main(int argc, char** argv) {
  tilelite_config conf;

  if (ini_parse("conf.ini", ini_parse_callback, &conf) < 0) {
    fprintf(stderr, "failed load configuration file\n");
    return 1;
  }

  set_defaults(&conf);
  set_signal_handler(SIGPIPE, SIG_IGN);
  set_signal_handler(SIGINT, SIG_IGN);

  int sfd = bind_tcp(conf["port"].c_str());
  if (sfd == -1) {
    return 1;
  }

  if (listen(sfd, SOMAXCONN) == -1) {
    perror("listen");
    return 1;
  }

#ifdef TILELITE_EPOLL
  ev_loop_epoll loop;
  ev_loop_epoll_init(&loop, sfd);
#elif TILELITE_KQUEUE
  ev_loop_kqueue loop;
  ev_loop_kqueue_init(&loop, sfd);
#endif

  auto context = std::unique_ptr<tilelite>(new tilelite(&conf));
  loop.user = context.get();

  auto read_callback = [](int fd, const char* data, int len, void* user) {
    tilelite* ctx = (tilelite*)user;

    tile t = { 0 };
    bool valid = read_request(data, len, &t);
    if (valid) {
      t.request_time = tl_usec_now();
      ctx->queue_tile_request({fd, t});
    }
  };

#ifdef TILELITE_EPOLL
  ev_loop_epoll_run(&loop, read_callback);
#elif TILELITE_KQUEUE
  ev_loop_kqueue_run(&loop, read_callback);
#endif

  return 0;
}
