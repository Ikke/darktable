/*
    This file is part of darktable,
    copyright (c) 2016-2019 pascal obry

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/darktable.h"
#include "common/collection.h"
#include "common/undo.h"
#include <glib.h>    // for GList, gpointer, g_list_first, g_list_prepend
#include <stdlib.h>  // for NULL, malloc, free
#include <sys/time.h>

const double MAX_TIME_PERIOD = 0.5; // in second

typedef struct dt_undo_item_t
{
  gpointer user_data;
  dt_undo_type_t type;
  dt_undo_data_t data;
  double ts;
  gboolean is_group;
  void (*undo)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data, dt_undo_action_t action);
  void (*free_data)(gpointer data);
} dt_undo_item_t;

dt_undo_t *dt_undo_init(void)
{
  dt_undo_t *udata = malloc(sizeof(dt_undo_t));
  udata->undo_list = NULL;
  udata->redo_list = NULL;
  udata->disable_next = FALSE;
  udata->locked = FALSE;
  dt_pthread_mutex_init(&udata->mutex, NULL);
  udata->group = DT_UNDO_NONE;
  udata->group_indent = 0;
  return udata;
}

#define LOCK \
  dt_pthread_mutex_lock(&self->mutex); self->locked = TRUE

#define UNLOCK \
  self->locked = FALSE; dt_pthread_mutex_unlock(&self->mutex)

void dt_undo_disable_next(dt_undo_t *self)
{
  self->disable_next = TRUE;
}

void dt_undo_cleanup(dt_undo_t *self)
{
  dt_undo_clear(self, DT_UNDO_ALL);
  dt_pthread_mutex_destroy(&self->mutex);
}

static void _free_undo_data(void *p)
{
  dt_undo_item_t *item = (dt_undo_item_t *)p;
  if (item->free_data) item->free_data(item->data);
  free(item);
}

static void _undo_record(dt_undo_t *self, gpointer user_data, dt_undo_type_t type, dt_undo_data_t data,
                         gboolean is_group,
                         void (*undo)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t item, dt_undo_action_t action),
                         void (*free_data)(gpointer data))
{
  if(!self) return;

  if(self->disable_next)
  {
    if(free_data) free_data(data);
    self->disable_next = FALSE;
  }
  else
  {
    // do not block, if an undo record is asked and there is a lock it means that this call has been done in un
    // undo/redo callback. We just skip this event.

    if(!self->locked)
    {
      LOCK;

      dt_undo_item_t *item = malloc(sizeof(dt_undo_item_t));

      item->user_data = user_data;
      item->type      = type;
      item->data      = data;
      item->undo      = undo;
      item->free_data = free_data;
      item->ts        = dt_get_wtime();
      item->is_group  = is_group;

      self->undo_list = g_list_prepend(self->undo_list, (gpointer)item);

      // recording an undo data invalidate all the redo
      g_list_free_full(self->redo_list, _free_undo_data);
      self->redo_list = NULL;

      UNLOCK;
    }
  }
}

void dt_undo_start_group(dt_undo_t *self, dt_undo_type_t type)
{
  if(!self) return;

  if(self->group == DT_UNDO_NONE)
  {
    self->group = type;
    self->group_indent = 1;
    _undo_record(self, NULL, type, NULL, TRUE, NULL, NULL);
  }
  else
    self->group_indent++;
}

void dt_undo_end_group(dt_undo_t *self)
{
  if(!self) return;

  assert(self->group_indent>0);
  self->group_indent--;
  if(self->group_indent == 0)
  {
    _undo_record(self, NULL, self->group, NULL, TRUE, NULL, NULL);
    self->group = DT_UNDO_NONE;
  }
}

void dt_undo_record(dt_undo_t *self, gpointer user_data, dt_undo_type_t type, dt_undo_data_t data,
                    void (*undo)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t item, dt_undo_action_t action),
                    void (*free_data)(gpointer data))
{
  _undo_record(self, user_data, type, data, FALSE, undo, free_data);
}

static void _undo_do_undo_redo(dt_undo_t *self, uint32_t filter, dt_undo_action_t action)
{
  if(!self) return;

  LOCK;

  // we take/remove item from the FROM list and add them into the TO list:
  GList **from = action == DT_ACTION_UNDO ? &self->undo_list : &self->redo_list;
  GList **to   = action == DT_ACTION_UNDO ? &self->redo_list : &self->undo_list;

  GList *l = g_list_first(*from);

  // check for first item that is matching the given pattern

  while(l)
  {
    dt_undo_item_t *item = (dt_undo_item_t *)l->data;

    if(item->type & filter)
    {
      if(item->is_group)
      {
        gboolean is_group = FALSE;

        GList *next = g_list_next(l);

        //  first move the group item into the TO list
        *from = g_list_remove(*from, item);
        *to   = g_list_prepend(*to, item);

        while((l = next) && !is_group)
        {
          item = (dt_undo_item_t *)l->data;
          next = g_list_next(l);

          //  first remove element from FROM list
          *from = g_list_remove(*from, item);

          //  callback with undo or redo data
          if(item->is_group)
            is_group = TRUE;
          else
            item->undo(item->user_data, item->type, item->data, action);

          //  add old position back into the TO list
          *to = g_list_prepend(*to, item);
        }
      }
      else
      {
        const double first_item_ts = item->ts;
        gboolean in_group = FALSE;

        //  when found, handle all items of the same type and in the same time period

        do
        {
          GList *next = g_list_next(l);

          //  first remove element from FROM list
          *from = g_list_remove(*from, item);

          if(item->is_group)
            in_group = !in_group;
          else
            //  callback with redo or redo data
            item->undo(item->user_data, item->type, item->data, action);

          //  add old position back into the TO list
          *to = g_list_prepend(*to, item);

          l = next;
          if (l) item = (dt_undo_item_t *)l->data;
        } while (l && (item->type & filter) && (in_group || (fabs(item->ts - first_item_ts) < MAX_TIME_PERIOD)));
      }

      break;
    }
    l = g_list_next(l);
  }
  UNLOCK;

  dt_collection_update_query(darktable.collection);
}

void dt_undo_do_redo(dt_undo_t *self, uint32_t filter)
{
  _undo_do_undo_redo(self, filter, DT_ACTION_REDO);
}

void dt_undo_do_undo(dt_undo_t *self, uint32_t filter)
{
  _undo_do_undo_redo(self, filter, DT_ACTION_UNDO);
}

static void _undo_clear_list(GList **list, uint32_t filter)
{
  GList *l = g_list_first(*list);

  // check for first item that is matching the given pattern

  while(l)
  {
    dt_undo_item_t *item = (dt_undo_item_t *)l->data;
    GList *next = l->next;
    if(item->type & filter)
    {
      //  remove this element
      *list = g_list_remove(*list, item);
      _free_undo_data((void *)item);
    }
    l = next;
  };
}

void dt_undo_clear(dt_undo_t *self, uint32_t filter)
{
  if(!self) return;

  LOCK;
  _undo_clear_list(&self->undo_list, filter);
  _undo_clear_list(&self->redo_list, filter);
  self->undo_list = NULL;
  self->redo_list = NULL;
  self->disable_next = FALSE;
  UNLOCK;
}

static void _undo_iterate(GList *list, uint32_t filter, gpointer user_data,
                          void (*apply)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t item))
{
  GList *l = g_list_first(list);

  // check for first item that is matching the given pattern

  while(l)
  {
    dt_undo_item_t *item = (dt_undo_item_t *)l->data;
    if(!item->is_group && (item->type & filter))
    {
      apply(user_data, item->type, item->data);
    }
    l = l->next;
  };
}

void dt_undo_iterate_internal(dt_undo_t *self, uint32_t filter, gpointer user_data,
                              void (*apply)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t item))
{
  if(!self) return;

  _undo_iterate(self->undo_list, filter, user_data, apply);
  _undo_iterate(self->redo_list, filter, user_data, apply);
}


void dt_undo_iterate(dt_undo_t *self, uint32_t filter, gpointer user_data,
                     void (*apply)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t item))
{
  if(!self) return;

  LOCK;
  dt_undo_iterate_internal(self, filter, user_data, apply);
  UNLOCK;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
