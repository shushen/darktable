/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#include "common/debug.h"
#include "common/styles.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/masks.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/styles.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "common/history.h"

DT_MODULE(1)


typedef struct dt_undo_history_t
{
  GList *before_snapshot, *after_snapshot;
  int before_end, after_end;
  GList *before_iop_order_list, *after_iop_order_list;
} dt_undo_history_t;

typedef struct dt_lib_history_t
{
  /* vbox with managed history items */
  GtkWidget *history_box;
  GtkWidget *create_button;
  GtkWidget *compress_button;
  gboolean record_undo;
  // previous_* below store values sent by signal DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE
  GList *previous_snapshot;
  int previous_history_end;
  GList *previous_iop_order_list;
} dt_lib_history_t;

/* 3 widgets in each history line */
#define HIST_WIDGET_NUMBER 0
#define HIST_WIDGET_MODULE 1
#define HIST_WIDGET_STATUS 2

/* compress history stack */
static void _lib_history_compress_clicked_callback(GtkWidget *widget, gpointer user_data);
static void _lib_history_button_clicked_callback(GtkWidget *widget, gpointer user_data);
static void _lib_history_create_style_button_clicked_callback(GtkWidget *widget, gpointer user_data);
/* signal callback for history change */
static void _lib_history_will_change_callback(gpointer instance, GList *history, int history_end,
                                              GList *iop_order_list, gpointer user_data);
static void _lib_history_change_callback(gpointer instance, gpointer user_data);
static void _lib_history_module_remove_callback(gpointer instance, dt_iop_module_t *module, gpointer user_data);

const char *name(dt_lib_module_t *self)
{
  return _("history");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 900;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "create style from history"), 0, 0);
//   dt_accel_register_lib(self, NC_("accel", "apply style from popup menu"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "compress history stack"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_history_t *d = (dt_lib_history_t *)self->data;

  dt_accel_connect_button_lib(self, "create style from history", d->create_button);
//   dt_accel_connect_button_lib(self, "apply style from popup menu", d->apply_button);
  dt_accel_connect_button_lib(self, "compress history stack", d->compress_button);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_history_t *d = (dt_lib_history_t *)g_malloc0(sizeof(dt_lib_history_t));
  self->data = (void *)d;

  d->record_undo = TRUE;
  d->previous_snapshot = NULL;
  d->previous_history_end = 0;
  d->previous_iop_order_list = NULL;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));
  gtk_widget_set_name(self->widget, "history-ui");
  d->history_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *hhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  d->compress_button = gtk_button_new_with_label(_("compress history stack"));
  gtk_widget_set_tooltip_text(d->compress_button, _("create a minimal history stack which produces the same image"));
  g_signal_connect(G_OBJECT(d->compress_button), "clicked", G_CALLBACK(_lib_history_compress_clicked_callback), self);

  /* add toolbar button for creating style */
  d->create_button = dtgtk_button_new(dtgtk_cairo_paint_styles, CPF_NONE, NULL);
  g_signal_connect(G_OBJECT(d->create_button), "clicked",
                   G_CALLBACK(_lib_history_create_style_button_clicked_callback), NULL);
  gtk_widget_set_name(d->create_button, "non-flat");
  gtk_widget_set_tooltip_text(d->create_button, _("create a style from the current history stack"));

  /* add buttons to buttonbox */
  gtk_box_pack_start(GTK_BOX(hhbox), d->compress_button, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hhbox), d->create_button, FALSE, FALSE, 0);

  /* add history list and buttonbox to widget */
  gtk_box_pack_start(GTK_BOX(self->widget), d->history_box, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hhbox, FALSE, FALSE, 0);


  gtk_widget_show_all(self->widget);

  /* connect to history change signal for updating the history view */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE,
                            G_CALLBACK(_lib_history_will_change_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE,
                            G_CALLBACK(_lib_history_change_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_MODULE_REMOVE,
                            G_CALLBACK(_lib_history_module_remove_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_history_change_callback), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_history_module_remove_callback), self);
  g_free(self->data);
  self->data = NULL;
}

static GtkWidget *_lib_history_create_button(dt_lib_module_t *self, int num, const char *label,
                                             gboolean enabled, gboolean default_enabled, gboolean always_on, gboolean selected, gboolean deprecated)
{
  /* create label */
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gchar numlab[10];

  g_snprintf(numlab, sizeof(numlab), "%2d", num + 1);
  GtkWidget *numwidget = gtk_label_new(numlab);
  gtk_widget_set_name(numwidget, "history-number");

  GtkWidget *onoff = NULL;

  /* create toggle button */
  GtkWidget *widget = gtk_toggle_button_new_with_label(label);
  gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(widget)), GTK_ALIGN_START);

  if(always_on)
  {
    onoff = dtgtk_button_new(dtgtk_cairo_paint_switch_on, CPF_STYLE_FLAT | CPF_BG_TRANSPARENT, NULL);
    gtk_widget_set_name(onoff, "history-switch-always-enabled");
    gtk_widget_set_name(widget, "history-button-always-enabled");
    dtgtk_button_set_active(DTGTK_BUTTON(onoff), TRUE);
    gtk_widget_set_tooltip_text(onoff, _("always-on module"));
  }
  else if(default_enabled)
  {
    onoff = dtgtk_button_new(dtgtk_cairo_paint_switch, CPF_STYLE_FLAT | CPF_BG_TRANSPARENT, NULL);
    gtk_widget_set_name(onoff, "history-switch-default-enabled");
    gtk_widget_set_name(widget, "history-button-default-enabled");
    dtgtk_button_set_active(DTGTK_BUTTON(onoff), enabled);
    gtk_widget_set_tooltip_text(onoff, _("default enabled module"));
  }
  else
  {
    if(deprecated)
    {
      onoff = dtgtk_button_new(dtgtk_cairo_paint_switch_deprecated, CPF_STYLE_FLAT | CPF_BG_TRANSPARENT, NULL);
      gtk_widget_set_name(onoff, "history-switch-deprecated");
      gtk_widget_set_tooltip_text(onoff, _("deprecated module"));
    }
    else
    {
      onoff = dtgtk_button_new(dtgtk_cairo_paint_switch, CPF_STYLE_FLAT | CPF_BG_TRANSPARENT, NULL);
      gtk_widget_set_name(onoff, enabled ? "history-switch-enabled" : "history-switch");
    }
    gtk_widget_set_name(widget, enabled ? "history-button-enabled" : "history-button");
    dtgtk_button_set_active(DTGTK_BUTTON(onoff), enabled);
  }

  gtk_widget_set_sensitive (onoff, FALSE);

  g_object_set_data(G_OBJECT(widget), "history_number", GINT_TO_POINTER(num + 1));
  g_object_set_data(G_OBJECT(widget), "label", (gpointer)label);
  if(selected) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), TRUE);

  /* set callback when clicked */
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(_lib_history_button_clicked_callback), self);

  /* associate the history number */
  g_object_set_data(G_OBJECT(widget), "history-number", GINT_TO_POINTER(num + 1));

  gtk_box_pack_start(GTK_BOX(hbox), numwidget, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(hbox), onoff, FALSE, FALSE, 0);

  return hbox;
}

static void _reset_module_instance(GList *hist, dt_iop_module_t *module, int multi_priority)
{
  while (hist)
  {
    dt_dev_history_item_t *hit = (dt_dev_history_item_t *)hist->data;

    if(!hit->module && strcmp(hit->op_name, module->op) == 0 && hit->multi_priority == multi_priority)
    {
      hit->module = module;
    }
    hist = hist->next;
  }
}

struct _cb_data
{
  dt_iop_module_t *module;
  int multi_priority;
};

static void _undo_items_cb(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data)
{
  struct _cb_data *udata = (struct _cb_data *)user_data;
  dt_undo_history_t *hdata = (dt_undo_history_t *)data;
  _reset_module_instance(hdata->after_snapshot, udata->module, udata->multi_priority);
}

static void _history_invalidate_cb(gpointer user_data, dt_undo_type_t type, dt_undo_data_t item)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  dt_undo_history_t *hist = (dt_undo_history_t *)item;
  dt_dev_invalidate_history_module(hist->after_snapshot, module);
}

static void _add_module_expander(GList *iop_list, dt_iop_module_t *module)
{
  // dt_dev_reload_history_items won't do this for base instances
  // and it will call gui_init() for the rest
  // so we do it here
  if(!dt_iop_is_hidden(module) && !module->expander)
  {
      /* add module to right panel */
      GtkWidget *expander = dt_iop_gui_get_expander(module);
      dt_ui_container_add_widget(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER, expander);
      dt_iop_gui_set_expanded(module, TRUE, FALSE);
      dt_iop_gui_update_blending(module);
  }
}

// return the 1st history entry that matches module
static dt_dev_history_item_t *_search_history_by_module(GList *history_list, dt_iop_module_t *module)
{
  dt_dev_history_item_t *hist_ret = NULL;

  GList *history = g_list_first(history_list);
  while(history)
  {
    dt_dev_history_item_t *hist_item = (dt_dev_history_item_t *)history->data;

    if(hist_item->module == module)
    {
      hist_ret = hist_item;
      break;
    }

    history = g_list_next(history);
  }
  return hist_ret;
}

static int _check_deleted_instances(dt_develop_t *dev, GList **_iop_list, GList *history_list)
{
  GList *iop_list = *_iop_list;
  int deleted_module_found = 0;

  // we will check on dev->iop if there's a module that is not in history
  GList *modules = g_list_first(iop_list);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

    int delete_module = 0;

    // base modules are a special case
    // most base modules won't be in history and must not be deleted
    // but the user may have deleted a base instance of a multi-instance module
    // and then undo and redo, so we will end up with two entries in dev->iop
    // with multi_priority == 0, this can't happen and the extra one must be deleted
    // dev->iop is sorted by (priority, multi_priority DESC), so if the next one is
    // a base instance too, one must be deleted
    if(mod->multi_priority == 0)
    {
      GList *modules_next = g_list_next(modules);
      if(modules_next)
      {
        dt_iop_module_t *mod_next = (dt_iop_module_t *)modules_next->data;
        if(strcmp(mod_next->op, mod->op) == 0 && mod_next->multi_priority == 0)
        {
          // is the same one, check which one must be deleted
          const int mod_in_history = (_search_history_by_module(history_list, mod) != NULL);
          const int mod_next_in_history = (_search_history_by_module(history_list, mod_next) != NULL);

          // current is in history and next is not, delete next
          if(mod_in_history && !mod_next_in_history)
          {
            mod = mod_next;
            modules = modules_next;
            delete_module = 1;
          }
          // current is not in history and next is, delete current
          else if(!mod_in_history && mod_next_in_history)
          {
            delete_module = 1;
          }
          else
          {
            if(mod_in_history && mod_next_in_history)
              fprintf(
                  stderr,
                  "[_check_deleted_instances] found duplicate module %s %s (%i) and %s %s (%i) both in history\n",
                  mod->op, mod->multi_name, mod->multi_priority, mod_next->op, mod_next->multi_name,
                  mod_next->multi_priority);
            else
              fprintf(
                  stderr,
                  "[_check_deleted_instances] found duplicate module %s %s (%i) and %s %s (%i) none in history\n",
                  mod->op, mod->multi_name, mod->multi_priority, mod_next->op, mod_next->multi_name,
                  mod_next->multi_priority);
          }
        }
      }
    }
    // this is a regular multi-instance and must be in history
    else
    {
      delete_module = (_search_history_by_module(history_list, mod) == NULL);
    }

    // if module is not in history we delete it
    if(delete_module)
    {
      deleted_module_found = 1;

      if(darktable.develop->gui_module == mod) dt_iop_request_focus(NULL);

      ++darktable.gui->reset;

      // we remove the plugin effectively
      if(!dt_iop_is_hidden(mod))
      {
        // we just hide the module to avoid lots of gtk critical warnings
        gtk_widget_hide(mod->expander);

        // this is copied from dt_iop_gui_delete_callback(), not sure why the above sentence...
        gtk_widget_destroy(mod->widget);
        dt_iop_gui_cleanup_module(mod);
      }

      iop_list = g_list_remove_link(iop_list, modules);

      // remove it from all snapshots
      dt_undo_iterate_internal(darktable.undo, DT_UNDO_HISTORY, mod, &_history_invalidate_cb);

      // we cleanup the module
      dt_accel_disconnect_list(mod->accel_closures);
      dt_accel_cleanup_locals_iop(mod);
      mod->accel_closures = NULL;
      // don't delete the module, a pipe may still need it
      dev->alliop = g_list_append(dev->alliop, mod);

      --darktable.gui->reset;

      // and reset the list
      modules = g_list_first(iop_list);
      continue;
    }

    modules = g_list_next(modules);
  }
  if(deleted_module_found) iop_list = g_list_sort(iop_list, dt_sort_iop_by_order);

  *_iop_list = iop_list;

  return deleted_module_found;
}

static void _reorder_gui_module_list(dt_develop_t *dev)
{
  int pos_module = 0;
  GList *modules = g_list_last(dev->iop);
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    GtkWidget *expander = module->expander;
    if(expander)
    {
      gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER), expander,
                            pos_module++);
    }

    modules = g_list_previous(modules);
  }
}

static int _rebuild_multi_priority(GList *history_list)
{
  int changed = 0;
  GList *history = g_list_first(history_list);
  while(history)
  {
    dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)history->data;

    // if multi_priority is different in history and dev->iop
    // we keep the history version
    if(hitem->module && hitem->module->multi_priority != hitem->multi_priority)
    {
      dt_iop_update_multi_priority(hitem->module, hitem->multi_priority);
      changed = 1;
    }

    history = g_list_next(history);
  }
  return changed;
}

static int _create_deleted_modules(GList **_iop_list, GList *history_list)
{
  GList *iop_list = *_iop_list;
  int changed = 0;
  gboolean done = FALSE;

  GList *l = g_list_first(history_list);
  while(l)
  {
    GList *next = g_list_next(l);
    dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)l->data;

    // this fixes the duplicate module when undo: hitem->multi_priority = 0;
    if(hitem->module == NULL)
    {
      changed = 1;

      const dt_iop_module_t *base_module = dt_iop_get_module_from_list(iop_list, hitem->op_name);
      if(base_module == NULL)
      {
        fprintf(stderr, "[_create_deleted_modules] can't find base module for %s\n", hitem->op_name);
        return changed;
      }

      // from there we create a new module for this base instance. The goal is to do a very minimal setup of the
      // new module to be able to write the history items. From there we reload the whole history back and this
      // will recreate the proper module instances.
      dt_iop_module_t *module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
      if(dt_iop_load_module(module, base_module->so, base_module->dev))
      {
        return changed;
      }
      module->instance = base_module->instance;

      if(!dt_iop_is_hidden(module))
      {
        module->gui_init(module);
      }

      // adjust the multi_name of the new module
      g_strlcpy(module->multi_name, hitem->multi_name, sizeof(module->multi_name));
      dt_iop_update_multi_priority(module, hitem->multi_priority);
      module->iop_order = hitem->iop_order;

      // we insert this module into dev->iop
      iop_list = g_list_insert_sorted(iop_list, module, dt_sort_iop_by_order);

      // add the expander, dt_dev_reload_history_items() don't work well without one
      _add_module_expander(iop_list, module);

      // if not already done, set the module to all others same instance
      if(!done)
      {
        _reset_module_instance(history_list, module, hitem->multi_priority);

        // and do that also in the undo/redo lists
        struct _cb_data udata = { module, hitem->multi_priority };
        dt_undo_iterate_internal(darktable.undo, DT_UNDO_HISTORY, &udata, &_undo_items_cb);
        done = TRUE;
      }

      hitem->module = module;
    }
    l = next;
  }

  *_iop_list = iop_list;

  return changed;
}

static void _pop_undo(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data, dt_undo_action_t action, GList **imgs)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;

  if(type == DT_UNDO_HISTORY)
  {
    dt_lib_history_t *d = (dt_lib_history_t *)self->data;
    dt_undo_history_t *hist = (dt_undo_history_t *)data;
    dt_develop_t *dev = darktable.develop;

    // we will work on a copy of history and modules
    // when we're done we'll replace dev->history and dev->iop
    GList *history_temp = NULL;
    int hist_end = 0;

    if(action == DT_ACTION_UNDO)
    {
      history_temp = dt_history_duplicate(hist->before_snapshot);
      hist_end = hist->before_end;
      dev->iop_order_list = dt_ioppr_iop_order_copy_deep(hist->before_iop_order_list);
    }
    else
    {
      history_temp = dt_history_duplicate(hist->after_snapshot);
      hist_end = hist->after_end;
      dev->iop_order_list = dt_ioppr_iop_order_copy_deep(hist->after_iop_order_list);
    }

    GList *iop_temp = g_list_copy(dev->iop);

    // topology has changed?
    int pipe_remove = 0;

    // we have to check if multi_priority has changed since history was saved
    // we will adjust it here
    if(_rebuild_multi_priority(history_temp))
    {
      pipe_remove = 1;
      iop_temp = g_list_sort(iop_temp, dt_sort_iop_by_order);
    }

    // check if this undo a delete module and re-create it
    if(_create_deleted_modules(&iop_temp, history_temp))
    {
      pipe_remove = 1;
    }

    // check if this is a redo of a delete module or an undo of an add module
    if(_check_deleted_instances(dev, &iop_temp, history_temp))
    {
      pipe_remove = 1;
    }

    // disable recording undo as the _lib_history_change_callback will be triggered by the calls below
    d->record_undo = FALSE;

    dt_pthread_mutex_lock(&dev->history_mutex);

    // set history and modules to dev
    GList *history_temp2 = dev->history;
    dev->history = history_temp;
    dev->history_end = hist_end;
    g_list_free_full(history_temp2, dt_dev_free_history_item);
    GList *iop_temp2 = dev->iop;
    dev->iop = iop_temp;
    g_list_free(iop_temp2);

    // topology has changed
    if(pipe_remove)
    {
      // we refresh the pipe
      dev->pipe->changed |= DT_DEV_PIPE_REMOVE;
      dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;
      dev->preview2_pipe->changed |= DT_DEV_PIPE_REMOVE;
      dev->pipe->cache_obsolete = 1;
      dev->preview_pipe->cache_obsolete = 1;
      dev->preview2_pipe->cache_obsolete = 1;

      // invalidate buffers and force redraw of darkroom
      dt_dev_invalidate_all(dev);
    }

    dt_pthread_mutex_unlock(&dev->history_mutex);

    // if dev->iop has changed reflect that on module list
    if(pipe_remove) _reorder_gui_module_list(dev);

    // write new history and reload
    dt_dev_write_history(dev);
    dt_dev_reload_history_items(dev);

    dt_ioppr_resync_modules_order(dev);

    dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
  }
}

static void _history_undo_data_free(gpointer data)
{
  dt_undo_history_t *hist = (dt_undo_history_t *)data;
  g_list_free_full(hist->before_snapshot, dt_dev_free_history_item);
  g_list_free_full(hist->after_snapshot, dt_dev_free_history_item);
  g_list_free_full(hist->before_iop_order_list, free);
  g_list_free_full(hist->after_iop_order_list, free);
  free(data);
}

static void _lib_history_module_remove_callback(gpointer instance, dt_iop_module_t *module, gpointer user_data)
{
  dt_undo_iterate(darktable.undo, DT_UNDO_HISTORY, module, &_history_invalidate_cb);
}

static void _lib_history_will_change_callback(gpointer instance, GList *history, int history_end, GList *iop_order_list,
                                              gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_history_t *lib = (dt_lib_history_t *)self->data;

  if(lib->record_undo)
  {
    // history is about to change, we want here ot record a snapshot of the history for the undo
    // record previous history
    g_list_free_full(lib->previous_snapshot, free);
    g_list_free_full(lib->previous_iop_order_list, free);
    lib->previous_snapshot = history;
    lib->previous_history_end = history_end;
    lib->previous_iop_order_list = iop_order_list;
  }
}

static void _lib_history_change_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_history_t *d = (dt_lib_history_t *)self->data;

  /* first destroy all buttons in list */
  gtk_container_foreach(GTK_CONTAINER(d->history_box), (GtkCallback)gtk_widget_destroy, 0);

  /* add default which always should be */
  int num = -1;
  GtkWidget *widget =
    _lib_history_create_button(self, num, _("original"), FALSE, FALSE, TRUE, darktable.develop->history_end == 0, FALSE);
  gtk_box_pack_start(GTK_BOX(d->history_box), widget, TRUE, TRUE, 0);
  num++;

  if (d->record_undo == TRUE)
  {
    /* record undo/redo history snapshot */
    dt_undo_history_t *hist = malloc(sizeof(dt_undo_history_t));
    hist->before_snapshot = dt_history_duplicate(d->previous_snapshot);
    hist->before_end = d->previous_history_end;
    hist->before_iop_order_list = dt_ioppr_iop_order_copy_deep(d->previous_iop_order_list);

    hist->after_snapshot = dt_history_duplicate(darktable.develop->history);
    hist->after_end = darktable.develop->history_end;
    hist->after_iop_order_list = dt_ioppr_iop_order_copy_deep(darktable.develop->iop_order_list);

    dt_undo_record(darktable.undo, self, DT_UNDO_HISTORY, (dt_undo_data_t)hist,
                   _pop_undo, _history_undo_data_free);
  }
  else
    d->record_undo = TRUE;

  /* lock history mutex */
  dt_pthread_mutex_lock(&darktable.develop->history_mutex);

  /* iterate over history items and add them to list*/
  GList *history = g_list_first(darktable.develop->history);
  while(history)
  {
    const dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)(history->data);
    gchar *label;
    if(!hitem->multi_name[0] || strcmp(hitem->multi_name, "0") == 0)
      label = g_strdup_printf("%s", hitem->module->name());
    else
      label = g_strdup_printf("%s %s", hitem->module->name(), hitem->multi_name);

    const gboolean selected = (num == darktable.develop->history_end - 1);
    widget =
      _lib_history_create_button(self, num, label, (hitem->enabled || (strcmp(hitem->op_name, "mask_manager") == 0)),
                                 hitem->module->default_enabled, hitem->module->hide_enable_button, selected,
                                 hitem->module->flags() & IOP_FLAGS_DEPRECATED);

    g_free(label);

    gtk_box_pack_start(GTK_BOX(d->history_box), widget, TRUE, TRUE, 0);
    gtk_box_reorder_child(GTK_BOX(d->history_box), widget, 0);
    num++;

    history = g_list_next(history);
  }

  /* show all widgets */
  gtk_widget_show_all(d->history_box);

  dt_pthread_mutex_unlock(&darktable.develop->history_mutex);
}

static void _lib_history_compress_clicked_callback(GtkWidget *widget, gpointer user_data)
{
  const int32_t imgid = darktable.develop->image_storage.id;
  if(!imgid) return;

  dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE,
                          dt_history_duplicate(darktable.develop->history), darktable.develop->history_end,
                          dt_ioppr_iop_order_copy_deep(darktable.develop->iop_order_list));

  // As dt_history_compress_on_image does *not* use the history stack data at all
  // make sure the current stack is in the database
  dt_dev_write_history(darktable.develop);

  dt_history_compress_on_image(imgid);

  sqlite3_stmt *stmt;

  // load new history and write it back to ensure that all history are properly numbered without a gap
  dt_dev_reload_history_items(darktable.develop);
  dt_dev_write_history(darktable.develop);
  dt_image_synch_xmp(imgid);

  // then we can get the item to select in the new clean-up history retrieve the position of the module
  // corresponding to the history end.
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT IFNULL(MAX(num)+1, 0) FROM main.history "
                                                             "WHERE imgid=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if (sqlite3_step(stmt) == SQLITE_ROW)
    darktable.develop->history_end = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // select the new history end corresponding to the one before the history compression
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "UPDATE main.images SET history_end=?2 WHERE id=?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, darktable.develop->history_end);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  dt_dev_reload_history_items(darktable.develop);
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
  dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
}

static void _lib_history_button_clicked_callback(GtkWidget *widget, gpointer user_data)
{
  static int reset = 0;
  if(reset) return;
  if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) return;

  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_history_t *d = (dt_lib_history_t *)self->data;
  reset = 1;

  /* deactivate all toggle buttons */
  GList *children = gtk_container_get_children(GTK_CONTAINER(d->history_box));
  for(GList *l = children; l != NULL; l = g_list_next(l))
  {
    GList *hbox = gtk_container_get_children(GTK_CONTAINER(l->data));
    GtkToggleButton *b = GTK_TOGGLE_BUTTON(g_list_nth(hbox, HIST_WIDGET_MODULE)->data);
    if(b != GTK_TOGGLE_BUTTON(widget)) g_object_set(G_OBJECT(b), "active", FALSE, (gchar *)0);
  }
  g_list_free(children);

  reset = 0;
  if(darktable.gui->reset) return;

  dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE,
                          dt_history_duplicate(darktable.develop->history), darktable.develop->history_end,
                          dt_ioppr_iop_order_copy_deep(darktable.develop->iop_order_list));

  /* revert to given history item. */
  const int num = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "history-number"));
  dt_dev_pop_history_items(darktable.develop, num);
  // set the module list order
  dt_dev_reorder_gui_module_list(darktable.develop);
  /* signal history changed */
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
  dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
}

static void _lib_history_create_style_button_clicked_callback(GtkWidget *widget, gpointer user_data)
{
  if(darktable.develop->image_storage.id)
  {
    dt_dev_write_history(darktable.develop);
    dt_gui_styles_dialog_new(darktable.develop->image_storage.id);
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
