/*
 * Copyright (c) 2012 Nagy, Attila <bra@fsn.hu>
 * Copyright (c) 2012-2013 BalaBit IT Ltd, Budapest, Hungary
 * Copyright (c) 2012-2013 Gergely Nagy <algernon@balabit.hu>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "afamqp.h"
#include "afamqp-parser.h"
#include "plugin.h"
#include "messages.h"
#include "misc.h"
#include "stats.h"
#include "nvtable.h"
#include "logqueue.h"
#include "scratch-buffers.h"
#include "plugin-types.h"
#include "logthrdestdrv.h"

#include <amqp.h>
#include <amqp_framing.h>

typedef struct
{
  LogThrDestDriver super;

  /* Shared between main/writer; only read by the writer, never written */
  gchar *exchange;
  gchar *exchange_type;
  LogTemplate *routing_key_template;
  LogTemplate *body_template;

  gboolean declare;
  gint persistent;

  gchar *vhost;
  gchar *host;
  gint port;

  gchar *user;
  gchar *password;

  LogTemplateOptions template_options;
  ValuePairs *vp;

  /* Writer-only stuff */
  amqp_connection_state_t conn;
  amqp_table_entry_t *entries;
  gint32 max_entries;
  gint32 seq_num;
} AMQPDestDriver;

/*
 * Configuration
 */

void
afamqp_dd_set_user(LogDriver *d, const gchar *user)
{
  AMQPDestDriver *self = (AMQPDestDriver *) d;

  g_free(self->user);
  self->user = g_strdup(user);
}

void
afamqp_dd_set_password(LogDriver *d, const gchar *password)
{
  AMQPDestDriver *self = (AMQPDestDriver *) d;

  g_free(self->password);
  self->password = g_strdup(password);
}

void
afamqp_dd_set_vhost(LogDriver *d, const gchar *vhost)
{
  AMQPDestDriver *self = (AMQPDestDriver *) d;

  g_free(self->vhost);
  self->vhost = g_strdup(vhost);
}

void
afamqp_dd_set_host(LogDriver *d, const gchar *host)
{
  AMQPDestDriver *self = (AMQPDestDriver *) d;

  g_free(self->host);
  self->host = g_strdup(host);
}

void
afamqp_dd_set_port(LogDriver *d, gint port)
{
  AMQPDestDriver *self = (AMQPDestDriver *) d;

  self->port = (int) port;
}

void
afamqp_dd_set_exchange(LogDriver *d, const gchar *exchange)
{
  AMQPDestDriver *self = (AMQPDestDriver *) d;

  g_free(self->exchange);
  self->exchange = g_strdup(exchange);
}

void
afamqp_dd_set_exchange_declare(LogDriver *d, gboolean declare)
{
  AMQPDestDriver *self = (AMQPDestDriver *) d;

  self->declare = declare;
}

void
afamqp_dd_set_exchange_type(LogDriver *d, const gchar *exchange_type)
{
  AMQPDestDriver *self = (AMQPDestDriver *) d;

  g_free(self->exchange_type);
  self->exchange_type = g_strdup(exchange_type);
}

void
afamqp_dd_set_routing_key(LogDriver *d, const gchar *routing_key)
{
  AMQPDestDriver *self = (AMQPDestDriver *) d;

  log_template_compile(self->routing_key_template, routing_key, NULL);
}

void
afamqp_dd_set_body(LogDriver *d, const gchar *body)
{
  AMQPDestDriver *self = (AMQPDestDriver *) d;

  if (!self->body_template)
    self->body_template = log_template_new(configuration, NULL);
  log_template_compile(self->body_template, body, NULL);
}

void
afamqp_dd_set_persistent(LogDriver *s, gboolean persistent)
{
  AMQPDestDriver *self = (AMQPDestDriver *) s;

  if (persistent)
    self->persistent = 2;
  else
    self->persistent = 1;
}

void
afamqp_dd_set_value_pairs(LogDriver *d, ValuePairs *vp)
{
  AMQPDestDriver *self = (AMQPDestDriver *) d;

  if (self->vp)
    value_pairs_free(self->vp);
  self->vp = vp;
  value_pairs_set_template_options(vp, &self->template_options);
}

LogTemplateOptions *
afamqp_dd_get_template_options(LogDriver *s)
{
  AMQPDestDriver *self = (AMQPDestDriver *) s;

  return &self->template_options;
}

/*
 * Utilities
 */

static gchar *
afamqp_dd_format_stats_instance(LogThrDestDriver *s)
{
  AMQPDestDriver *self = (AMQPDestDriver *) s;
  static gchar persist_name[1024];

  g_snprintf(persist_name, sizeof(persist_name), "amqp,%s,%s,%u,%s,%s",
             self->vhost, self->host, self->port, self->exchange,
             self->exchange_type);
  return persist_name;
}

static gchar *
afamqp_dd_format_persist_name(LogThrDestDriver *s)
{
  AMQPDestDriver *self = (AMQPDestDriver *) s;
  static gchar persist_name[1024];

  g_snprintf(persist_name, sizeof(persist_name), "afamqp(%s,%s,%u,%s,%s)",
             self->vhost, self->host, self->port, self->exchange,
             self->exchange_type);
  return persist_name;
}

static void
afamqp_dd_disconnect(LogThrDestDriver *s)
{
  AMQPDestDriver *self = (AMQPDestDriver *)s;

  amqp_channel_close(self->conn, 1, AMQP_REPLY_SUCCESS);
  amqp_connection_close(self->conn, AMQP_REPLY_SUCCESS);
  amqp_destroy_connection(self->conn);
  self->conn = NULL;
}

static gboolean
afamqp_is_ok(AMQPDestDriver *self, gchar *context, amqp_rpc_reply_t ret)
{
  switch (ret.reply_type)
    {
    case AMQP_RESPONSE_NORMAL:
      break;

    case AMQP_RESPONSE_NONE:
      msg_error(context,
                evt_tag_str("driver", self->super.super.super.id),
                evt_tag_str("error", "missing RPC reply type"),
                evt_tag_int("time_reopen", self->super.time_reopen),
                NULL);
      log_threaded_dest_driver_suspend(&self->super);
      return FALSE;

    case AMQP_RESPONSE_LIBRARY_EXCEPTION:
      {
        gchar *errstr = amqp_error_string(ret.library_error);
        msg_error(context,
                  evt_tag_str("driver", self->super.super.super.id),
                  evt_tag_str("error", errstr),
                  evt_tag_int("time_reopen", self->super.time_reopen),
                  NULL);
        g_free (errstr);
        log_threaded_dest_driver_suspend(&self->super);
        return FALSE;
      }

    case AMQP_RESPONSE_SERVER_EXCEPTION:
      switch (ret.reply.id)
        {
        case AMQP_CONNECTION_CLOSE_METHOD:
          {
            amqp_connection_close_t *m =
              (amqp_connection_close_t *) ret.reply.decoded;
            msg_error(context,
                      evt_tag_str("driver", self->super.super.super.id),
                      evt_tag_str("error", "server connection error"),
                      evt_tag_int("code", m->reply_code),
                      evt_tag_str("text", m->reply_text.bytes),
                      evt_tag_int("time_reopen", self->super.time_reopen),
                      NULL);
            log_threaded_dest_driver_suspend(&self->super);
            return FALSE;
          }
        case AMQP_CHANNEL_CLOSE_METHOD:
          {
            amqp_channel_close_t *m =
              (amqp_channel_close_t *) ret.reply.decoded;
            msg_error(context,
                      evt_tag_str("driver", self->super.super.super.id),
                      evt_tag_str("error", "server channel error"),
                      evt_tag_int("code", m->reply_code),
                      evt_tag_str("text", m->reply_text.bytes),
                      evt_tag_int("time_reopen", self->super.time_reopen),
                      NULL);
            log_threaded_dest_driver_suspend(&self->super);
            return FALSE;
          }
        default:
          msg_error(context,
                    evt_tag_str("driver", self->super.super.super.id),
                    evt_tag_str("error", "unknown server error"),
                    evt_tag_printf("method id", "0x%08X", ret.reply.id),
                    evt_tag_int("time_reopen", self->super.time_reopen),
                    NULL);
          log_threaded_dest_driver_suspend(&self->super);
          return FALSE;
        }
      return FALSE;
    }
  return TRUE;
}

static gboolean
afamqp_dd_connect(AMQPDestDriver *self, gboolean reconnect)
{
  int sockfd;
  amqp_rpc_reply_t ret;

  if (reconnect && self->conn)
    {
      ret = amqp_get_rpc_reply(self->conn);
      if (ret.reply_type == AMQP_RESPONSE_NORMAL)
        return TRUE;
    }

  self->conn = amqp_new_connection();
  sockfd = amqp_open_socket(self->host, self->port);
  if (sockfd < 0)
    {
      gchar *errstr = amqp_error_string(-sockfd);
      msg_error("Error connecting to AMQP server",
                evt_tag_str("driver", self->super.super.super.id),
                evt_tag_str("error", errstr),
                evt_tag_int("time_reopen", self->super.time_reopen),
                NULL);
      g_free(errstr);
      return FALSE;
    }
  amqp_set_sockfd(self->conn, sockfd);

  ret = amqp_login(self->conn, self->vhost, 0, 131072, 0,
                   AMQP_SASL_METHOD_PLAIN, self->user, self->password);
  if (!afamqp_is_ok(self, "Error during AMQP login", ret))
    return FALSE;

  amqp_channel_open(self->conn, 1);
  ret = amqp_get_rpc_reply(self->conn);
  if (!afamqp_is_ok(self, "Error during AMQP channel open", ret))
    return FALSE;

  if (self->declare)
    {
      amqp_exchange_declare(self->conn, 1, amqp_cstring_bytes(self->exchange),
                            amqp_cstring_bytes(self->exchange_type), 0, 0,
                            amqp_empty_table);
      ret = amqp_get_rpc_reply(self->conn);
      if (!afamqp_is_ok(self, "Error during AMQP exchange declaration", ret))
        return FALSE;
    }

  msg_debug ("Connecting to AMQP succeeded",
             evt_tag_str("driver", self->super.super.super.id),
             NULL);

  return TRUE;
}

/*
 * Worker thread
 */

static gboolean
afamqp_vp_foreach(const gchar *name,
                  TypeHint type, const gchar *value,
                  gpointer user_data)
{
  amqp_table_entry_t **entries = (amqp_table_entry_t **) ((gpointer *)user_data)[0];
  gint *pos = (gint *) ((gpointer *)user_data)[1];
  gint32 *max_size = (gint32 *) ((gpointer *)user_data)[2];

  if (*pos == *max_size)
    {
      *max_size *= 2;
      *entries = g_renew(amqp_table_entry_t, *entries, *max_size);
    }

  (*entries)[*pos].key = amqp_cstring_bytes(strdup(name));
  (*entries)[*pos].value.kind = AMQP_FIELD_KIND_UTF8;
  (*entries)[*pos].value.value.bytes = amqp_cstring_bytes(strdup(value));

  (*pos)++;

  return FALSE;
}

static gboolean
afamqp_worker_publish(AMQPDestDriver *self, LogMessage *msg)
{
  gint pos = 0, ret;
  amqp_table_t table;
  amqp_basic_properties_t props;
  gboolean success = TRUE;
  SBGString *routing_key = sb_gstring_acquire();
  SBGString *body = sb_gstring_acquire();
  amqp_bytes_t body_bytes = amqp_cstring_bytes("");

  gpointer user_data[] = { &self->entries, &pos, &self->max_entries };

  value_pairs_foreach(self->vp, afamqp_vp_foreach, msg, self->seq_num,
                      user_data);

  table.num_entries = pos;
  table.entries = self->entries;

  props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG
    | AMQP_BASIC_DELIVERY_MODE_FLAG | AMQP_BASIC_HEADERS_FLAG;
  props.content_type = amqp_cstring_bytes("text/plain");
  props.delivery_mode = self->persistent;
  props.headers = table;

  log_template_format(self->routing_key_template, msg, NULL, LTZ_LOCAL,
                      self->seq_num, NULL, sb_gstring_string(routing_key));

  if (self->body_template)
    {
      log_template_format(self->body_template, msg, NULL, LTZ_LOCAL,
                          self->seq_num, NULL, sb_gstring_string(body));
      body_bytes = amqp_cstring_bytes(sb_gstring_string(body)->str);
    }

  ret = amqp_basic_publish(self->conn, 1, amqp_cstring_bytes(self->exchange),
                           amqp_cstring_bytes(sb_gstring_string(routing_key)->str),
                           0, 0, &props, body_bytes);

  sb_gstring_release(routing_key);
  sb_gstring_release(body);

  if (ret < 0)
    {
      msg_error("Network error while inserting into AMQP server",
                evt_tag_str("driver", self->super.super.super.id),
                evt_tag_int("time_reopen", self->super.time_reopen), NULL);
      success = FALSE;
    }

  while (--pos >= 0)
    {
      amqp_bytes_free(self->entries[pos].key);
      amqp_bytes_free(self->entries[pos].value.value.bytes);
    }

  return success;
}

static gboolean
afamqp_worker_insert(LogThrDestDriver *s)
{
  AMQPDestDriver *self = (AMQPDestDriver *)s;
  gboolean success;
  LogMessage *msg;
  LogPathOptions path_options = LOG_PATH_OPTIONS_INIT;

  afamqp_dd_connect(self, TRUE);

  success = log_queue_pop_head(s->queue, &msg, &path_options, FALSE, FALSE);
  if (!success)
    return TRUE;

  msg_set_context(msg);
  success = afamqp_worker_publish (self, msg);
  msg_set_context(NULL);

  if (success)
    {
      stats_counter_inc(s->stored_messages);
      step_sequence_number(&self->seq_num);
      log_msg_ack(msg, &path_options);
      log_msg_unref(msg);
    }
  else
    log_queue_push_head(s->queue, msg, &path_options);

  return success;
}

static void
afamqp_worker_thread_init(LogThrDestDriver *d)
{
  AMQPDestDriver *self = (AMQPDestDriver *)d;

  afamqp_dd_connect(self, FALSE);
}

/*
 * Main thread
 */

static gboolean
afamqp_dd_init(LogPipe *s)
{
  AMQPDestDriver *self = (AMQPDestDriver *) s;
  GlobalConfig *cfg = log_pipe_get_config(s);

  if (!log_threaded_dest_driver_init_method(s))
    return FALSE;

  log_template_options_init(&self->template_options, cfg);

  msg_verbose("Initializing AMQP destination",
              evt_tag_str("vhost", self->vhost),
              evt_tag_str("host", self->host),
              evt_tag_int("port", self->port),
              evt_tag_str("exchange", self->exchange),
              evt_tag_str("exchange_type", self->exchange_type),
              NULL);

  log_threaded_dest_driver_start(&self->super);

  return TRUE;
}

static void
afamqp_dd_free(LogPipe *d)
{
  AMQPDestDriver *self = (AMQPDestDriver *) d;

  log_template_options_destroy(&self->template_options);

  g_free(self->exchange);
  g_free(self->exchange_type);
  log_template_unref(self->routing_key_template);
  log_template_unref(self->body_template);
  g_free(self->user);
  g_free(self->password);
  g_free(self->host);
  g_free(self->vhost);
  g_free(self->entries);
  if (self->vp)
    value_pairs_free(self->vp);

  log_threaded_dest_driver_free(d);
}

/*
 * Plugin glue.
 */

LogDriver *
afamqp_dd_new(GlobalConfig *cfg)
{
  AMQPDestDriver *self = g_new0(AMQPDestDriver, 1);

  log_threaded_dest_driver_init_instance(&self->super);

  self->super.super.super.super.init = afamqp_dd_init;
  self->super.super.super.super.free_fn = afamqp_dd_free;

  self->super.worker.thread_init = afamqp_worker_thread_init;
  self->super.worker.disconnect = afamqp_dd_disconnect;
  self->super.worker.insert = afamqp_worker_insert;

  self->super.format.stats_instance = afamqp_dd_format_stats_instance;
  self->super.format.persist_name = afamqp_dd_format_persist_name;
  self->super.stats_source = SCS_AMQP;

  self->routing_key_template = log_template_new(cfg, NULL);

  afamqp_dd_set_vhost((LogDriver *) self, "/");
  afamqp_dd_set_host((LogDriver *) self, "127.0.0.1");
  afamqp_dd_set_port((LogDriver *) self, 5672);
  afamqp_dd_set_exchange((LogDriver *) self, "syslog");
  afamqp_dd_set_exchange_type((LogDriver *) self, "fanout");
  afamqp_dd_set_routing_key((LogDriver *) self, "");
  afamqp_dd_set_persistent((LogDriver *) self, TRUE);
  afamqp_dd_set_exchange_declare((LogDriver *) self, FALSE);

  init_sequence_number(&self->seq_num);

  self->max_entries = 256;
  self->entries = g_new(amqp_table_entry_t, self->max_entries);

  log_template_options_defaults(&self->template_options);
  afamqp_dd_set_value_pairs(&self->super.super.super, value_pairs_new_default(cfg));

  return (LogDriver *) self;
}

extern CfgParser afamqp_dd_parser;

static Plugin afamqp_plugin =
{
  .type = LL_CONTEXT_DESTINATION,
  .name = "amqp",
  .parser = &afamqp_parser
};

gboolean
afamqp_module_init(GlobalConfig *cfg, CfgArgs *args)
{
  plugin_register(cfg, &afamqp_plugin, 1);
  return TRUE;
}

const ModuleInfo module_info =
{
  .canonical_name = "afamqp",
  .version = VERSION,
  .description = "The afamqp module provides AMQP destination support for syslog-ng.",
  .core_revision = SOURCE_REVISION, .plugins = &afamqp_plugin,
  .plugins_len = 1,
};