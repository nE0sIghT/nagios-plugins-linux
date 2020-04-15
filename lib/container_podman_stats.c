/*
 * License: GPLv3+
 * Copyright (c) 2020 Davide Madrisan <davide.madrisan@gmail.com>
 *
 * A library for checking for Podman metrics via varlink calls.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE		/* activate extra prototypes for glibc */
#endif

#include <assert.h>
#include <string.h>
#include <varlink.h>

#include "common.h"
#include "collection.h"
#include "container_podman.h"
#include "json_helpers.h"
#include "logging.h"
#include "messages.h"
#include "string-macros.h"
#include "units.h"
#include "xalloc.h"
#include "xasprintf.h"
#include "xstrtol.h"

/* Hide all jsmn API symbols by making them static */
#define JSMN_STATIC
#include "jsmn.h"

/* parse the json stream containing the statistics for the container
   with the given id. The format of the data follows:

       {
         "container": {
           "block_input": 16601088,
           "block_output": 16384,
           "cpu": 1.0191267811342149e-07,
           "cpu_nano": 1616621000,
           "id": "e15712d1db8f92b3a00be9649345856da53ab22abcc8b22e286c5cd8fbf08c36",
           "mem_limit": 8232525824,
           "mem_perc": 0.10095059739468847,
           "mem_usage": 8310784,
           "name": "srv-redis-1",
           "net_input": 1048,
           "net_output": 7074,
           "pids": 4,
           "system_nano": 1586280558931850500
         }
       }	*/

static void
json_parser_stats (struct podman_varlink *pv, const char *id,
		   container_stats_t * stats)
{
  const char *varlinkmethod = "io.podman.GetContainerStats";
  char *errmsg = NULL, *json,
    *keys[] = { "mem_limit", "mem_usage", "name" },
    *vals[] = { NULL, NULL, NULL },
    **mem_limit = &vals[0], **mem_usage = &vals[1], **name = &vals[2],
    *root_key = "container", param[80];
  size_t i, ntoken, level = 0, keys_num = sizeof (keys) / sizeof (char *);
  jsmntok_t *tokens;
  int ret;

  stats->mem_limit = 0;
  stats->mem_usage = 0;

  sprintf (param, "{\"name\":\"%s\"}", id);
  dbg ("%s: parameter %s will be passed to podman_varlink_get()\n", __func__,
       param);
  ret = podman_varlink_get (pv, varlinkmethod, param, &json, &errmsg);
  if (ret < 0)
    {
#ifndef NPL_TESTING
      podman_varlink_unref (pv);
      plugin_error (STATE_UNKNOWN, 0, "%s", errmsg);
#else
      plugin_error (STATE_UNKNOWN, 0, "podman_varlink_get has failed");
#endif
    }
  dbg ("varlink %s returned: %s", varlinkmethod, json);

  tokens = json_tokenise (json, &ntoken);
  if (NULL == tokens)
    plugin_error (STATE_UNKNOWN, 0, "invalid or corrupted JSON data");

  for (i = 0; i < ntoken; i++)
    {
      jsmntok_t *t = &tokens[i];

      // should never reach uninitialized tokens
      assert (t->start != -1 && t->end != -1);

      dbg ("[%lu] %s: \"%.*s\"\n", i,
	   (t->type == JSMN_ARRAY) ? "JSMN_ARRAY" :
	   ((t->type == JSMN_OBJECT) ? "JSMN_OBJECT" :
	    ((t->type == JSMN_STRING) ? "JSMN_STRING" :
	     ((t->type ==
	       JSMN_UNDEFINED) ? "JSMN_UNDEFINED" : "JSMN_PRIMITIVE"))),
	   t->end - t->start, json + t->start);

      switch (t->type)
	{
	case JSMN_OBJECT:
	  if (level < 2)
	    level++;
	  break;

	case JSMN_STRING:
	  if ((1 == level) && (0 != json_token_streq (json, t, root_key)))
	    plugin_error (STATE_UNKNOWN, 0,
			  "%s: expected string \"%s\" not found",
			  __func__, root_key);
	  for (size_t j = 0; j < keys_num; j++)
	    {
	      if (0 == json_token_streq (json, t, keys[j]))
		{
		  vals[j] = json_token_tostr (json, &tokens[++i]);
		  dbg
		    ("found token \"%s\" with value \"%s\" at position %d\n",
		     keys[j], vals[j], t->start);
		  break;
		}
	    }
	  break;

	default:
	  if (0 == level)
	    plugin_error (STATE_UNKNOWN, 0,
			  "%s: root element must be an object", __func__);
	}

      /* the memory limit and usage are reported in bytes */
      if (*mem_limit)
	stats->mem_limit = strtol_or_err (*mem_limit,
					  "failed to parse mem_limit counter");
      if (*mem_usage)
	stats->mem_usage = strtol_or_err (*mem_usage,
					  "failed to parse mem_usage counter");
      if (*name)
	stats->name = xstrdup (*name);
    }

  assert (NULL != mem_limit);
  assert (NULL != mem_usage);
  assert (NULL != name);

  dbg ("%s: container memory: %lu/%lu\n", __func__, stats->mem_usage,
       stats->mem_limit);
  dbg ("%s: container name: %s\n", __func__, stats->name);

  free (tokens);
}

/* parse the json stream and return a pointer to the hashtable containing
   the values of the discovered 'tokens', or return NULL if the data
   cannot be parsed;

   the format of the data follows:

       {
         "containers": [
           {
             "command": [
               ...
             ],
             "containerrunning": true,
             "createdat": "2020-04-06T00:22:29+02:00",
             "id": "3b395e067a3071ba77b2b44999235589a0957c72e99d1186ff1e53a0069da727",
             "image": "docker.io/library/redis:latest",
             "imageid": "f0453552d7f26fc38ffc05fa034aa7a7bc6fbb01bc7bc5a9e4b3c0ab87068627",
             "mounts": [
                ...
             ],
             "names": "srv-redis-1",
             "namespaces": {
               ...
             },
             "ports": [
               ...
             ],
             "rootfssize": 98203942,
             "runningfor": "52.347336247s",
             "rwsize": 0,
             "status": "running"
           },
           {
             ...
           }
         ]
       }	*/

static void
json_parser_list (struct podman_varlink *pv, const char *image_name,
		  hashtable_t ** hashtable)
{
  char *errmsg = NULL, *json,
    *keys[] = { "containerrunning", "id", "image" },
    *vals[] = { NULL, NULL, NULL },
    **containerrunning = &vals[0], **id = &vals[1], **image = &vals[2];
  const char *root_key = "containers";
  size_t i, ntoken, level = 0, keys_num = sizeof (keys) / sizeof (char *);
  jsmntok_t *tokens;
  int ret;

  const char *varlinkmethod = "io.podman.ListContainers";
  *hashtable = counter_create ();

  ret = podman_varlink_get (pv, varlinkmethod, NULL, &json, &errmsg);
  if (ret < 0)
    {
#ifndef NPL_TESTING
      podman_varlink_unref (pv);
      plugin_error (STATE_UNKNOWN, 0, "%s", errmsg);
#else
      plugin_error (STATE_UNKNOWN, 0, "podman_varlink_get has failed");
#endif
    }
  dbg ("varlink %s returned: %s", varlinkmethod, json);

  tokens = json_tokenise (json, &ntoken);
  if (NULL == tokens)
    plugin_error (STATE_UNKNOWN, 0, "invalid or corrupted JSON data");

  for (i = 0; i < ntoken; i++)
    {
      jsmntok_t *t = &tokens[i];

      // should never reach uninitialized tokens
      assert (t->start != -1 && t->end != -1);

      dbg ("[%lu] %s: \"%.*s\"\n", i,
	   (t->type == JSMN_ARRAY) ? "JSMN_ARRAY" :
	   ((t->type == JSMN_OBJECT) ? "JSMN_OBJECT" :
	    ((t->type == JSMN_STRING) ? "JSMN_STRING" :
	     ((t->type ==
	       JSMN_UNDEFINED) ? "JSMN_UNDEFINED" : "JSMN_PRIMITIVE"))),
	   t->end - t->start, json + t->start);

      switch (t->type)
	{
	case JSMN_OBJECT:
	  level++;		/* FIXME: should be decremented at each object end */
	  break;

	case JSMN_STRING:
	  if ((1 == level) && (0 != json_token_streq (json, t, root_key)))
	    plugin_error (STATE_UNKNOWN, 0,
			  "%s: expected string \"%s\" not found",
			  __func__, root_key);
	  for (size_t j = 0; j < keys_num; j++)
	    {
	      if (0 == json_token_streq (json, t, keys[j]))
		{
		  vals[j] = json_token_tostr (json, &tokens[++i]);
		  dbg
		    ("found token \"%s\" with value \"%s\" at position %d\n",
		     keys[j], vals[j], t->start);
		  break;
		}
	    }
	  break;

	default:
	  if (0 == level)
	    plugin_error (STATE_UNKNOWN, 0,
			  "%s: root element must be an object", __func__);
	}

      if (podman_array_is_full (vals, keys_num))
	{
	  if (STREQ (*containerrunning, "true"))
	    {
	      if (image_name && STRNEQ (*image, image_name))
		dbg ("the container name does not match with %s\n",
		     image_name);
	      else
		{
		  char shortid[PODMAN_SHORTID_LEN];
		  podman_shortid (*id, shortid);
		  dbg ("(running) container id: %s (%s)\n", *id, shortid);
		  counter_put (*hashtable, shortid, 1);
		}
	    }
	  dbg ("new container found:\n");
	  for (size_t j = 0; j < keys_num; j++)
	    {
	      dbg (" * \"%s\": \"%s\"\n", keys[j], vals[j]);
	      vals[j] = NULL;
	    }
	}
    }

  free (tokens);
}

int
podman_stats (struct podman_varlink *pv, unsigned long long *tot_memory,
	      unit_shift shift, const char *image, char **status,
	      char **perfdata)
{
  char *tot_memory_str;
  size_t size;
  unsigned int containers;
  hashtable_t * hashtable;

  FILE *stream = open_memstream (perfdata, &size);

  json_parser_list (pv, image, &hashtable);

  *tot_memory = 0;
  containers = counter_get_unique_elements (hashtable);

  for (unsigned int j = 0; j < containers; j++)
    {
      char *shortid = hashtable->keys[j];
      container_stats_t stats;

      json_parser_stats (pv, shortid, &stats);
      fprintf (stream, "%s=%lukB;;;0;%lu ", stats.name,
	       (stats.mem_usage / 1000), (stats.mem_limit / 1000));
      *tot_memory += stats.mem_usage;
      free (stats.name);
    }
  fclose (stream);

  switch (shift)
    {
    default:
    case b_shift:
      tot_memory_str = xasprintf ("%lluB", *tot_memory);
      break;
    case k_shift:
      tot_memory_str = xasprintf ("%llukB", (*tot_memory) / 1000);
      break;
    case m_shift:
      tot_memory_str = xasprintf ("%gMB", (*tot_memory) / 1000000.0);
      break;
    case g_shift:
      tot_memory_str = xasprintf ("%gGB", (*tot_memory) / 1000000000.0);
      break;
  }

  *status =
    xasprintf ("%s of memory used by %u running containers"
	       , tot_memory_str
	       , containers);

  free (hashtable);
  free (tot_memory_str);

  return 0;
}
