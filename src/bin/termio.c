#include "private.h"
#include <Ecore_IMF.h>
#include <Ecore_IMF_Evas.h>
#include <Elementary.h>
#include <Ecore_Input.h>
#include "termio.h"
#include "termiolink.h"
#include "termpty.h"
#include "termcmd.h"
#include "utf8.h"
#include "col.h"
#include "keyin.h"
#include "config.h"
#include "utils.h"
#include "media.h"
#include "dbus.h"

typedef struct _Termio Termio;

struct _Termio
{
   Evas_Object_Smart_Clipped_Data __clipped_data;
   struct {
      int size;
      const char *name;
      int chw, chh;
   } font;
   struct {
      int w, h;
      Evas_Object *obj;
   } grid;
   struct {
        Evas_Object *top, *bottom, *theme;
   } sel;
   struct {
      Evas_Object *obj;
      int x, y;
   } cursor;
   struct {
      int cx, cy;
      int button;
   } mouse;
   struct {
      char *string;
      int x1, y1, x2, y2;
      int suspend;
      Eina_List *objs;
      Evas_Object *ctxpopup;
      struct {
         Evas_Object *dndobj;
         Evas_Coord x, y;
         Eina_Bool down : 1;
         Eina_Bool dnd : 1;
         Eina_Bool dndobjdel : 1;
      } down;
   } link;
   int zoom_fontsize_start;
   int scroll;
   unsigned int last_keyup;
   Eina_List *mirrors;
   Eina_List *seq;
   Evas_Object *self;
   Evas_Object *event;
   Termpty *pty;
   Ecore_Animator *anim;
   Ecore_Timer *delayed_size_timer;
   Ecore_Timer *link_do_timer;
   Ecore_Timer *mouse_selection_scroll;
   Ecore_Job *mouse_move_job;
   Ecore_Timer *mouseover_delay;
   Evas_Object *win, *theme, *glayer;
   Config *config;
   Ecore_IMF_Context *imf;
   const char *sel_str;
   Eina_List *cur_chids;
   Ecore_Job *sel_reset_job;
   double set_sel_at;
   Elm_Sel_Type sel_type;
   Eina_Bool jump_on_change : 1;
   Eina_Bool jump_on_keypress : 1;
   Eina_Bool have_sel : 1;
   Eina_Bool noreqsize : 1;
   Eina_Bool composing : 1;
   Eina_Bool didclick : 1;
   Eina_Bool moved : 1;
   Eina_Bool bottom_right : 1;
   Eina_Bool top_left : 1;
   Eina_Bool reset_sel : 1;
   Eina_Bool debugwhite : 1;
};

static Evas_Smart *_smart = NULL;
static Evas_Smart_Class _parent_sc = EVAS_SMART_CLASS_INIT_NULL;

static Eina_List *terms = NULL;

static void _smart_calculate(Evas_Object *obj);
static void _smart_mirror_del(void *data, Evas *evas EINA_UNUSED, Evas_Object *obj, void *info EINA_UNUSED);
static void _lost_selection(void *data, Elm_Sel_Type selection);
static void _take_selection_text(Evas_Object *obj, Elm_Sel_Type type, const char *text);

static void
_sel_set(Evas_Object *obj, Eina_Bool enable)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN(sd);

   if (sd->pty->selection.is_active == enable) return;
   sd->pty->selection.is_active = enable;
   if (enable)
     evas_object_smart_callback_call(sd->win, "selection,on", NULL);
   else
     evas_object_smart_callback_call(sd->win, "selection,off", NULL);
}

static inline Eina_Bool
_should_inline(const Evas_Object *obj)
{
   const Config *config = termio_config_get(obj);
   const Evas *e;
   const Evas_Modifier *mods;

   if (!config->helper.inline_please) return EINA_FALSE;

   e = evas_object_evas_get(obj);
   mods = evas_key_modifier_get(e);

   if (evas_key_modifier_is_set(mods, "Control"))  return EINA_FALSE;

   return EINA_TRUE;
}

static void
_activate_link(Evas_Object *obj, Eina_Bool may_inline)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Config *config = termio_config_get(obj);
   char buf[PATH_MAX], *s, *escaped;
   const char *path = NULL, *cmd = NULL;
   Eina_Bool url = EINA_FALSE, email = EINA_FALSE, handled = EINA_FALSE;
   int type;
   
   EINA_SAFETY_ON_NULL_RETURN(sd);
   if (!config) return;
   if (!sd->link.string) return;
   if (link_is_url(sd->link.string))
     {
        if (casestartswith(sd->link.string, "file://"))
          // TODO: decode string: %XX -> char
          path = sd->link.string + sizeof("file://") - 1;
        else
          url = EINA_TRUE;
     }
   else if (sd->link.string[0] == '/')
     path = sd->link.string;
   else if (link_is_email(sd->link.string))
     email = EINA_TRUE;

   if (url && casestartswith(sd->link.string, "mailto:"))
     {
        email = EINA_TRUE;
        url = EINA_FALSE;
     }

   s = eina_str_escape(sd->link.string);
   if (!s) return;
   if (email)
     {
        const char *p = s;

        // run mail client
        cmd = "xdg-email";
        
        if ((config->helper.email) &&
            (config->helper.email[0]))
          cmd = config->helper.email;

        if (casestartswith(s, "mailto:"))
          p += sizeof("mailto:") - 1;

        escaped = ecore_file_escape_name(p);
        if (escaped)
          {
             snprintf(buf, sizeof(buf), "%s %s", cmd, escaped);
             free(escaped);
          }
     }
   else if (path)
     {
        // locally accessible file
        cmd = "xdg-open";
        
        escaped = ecore_file_escape_name(s);
        if (escaped)
          {
             type = media_src_type_get(sd->link.string);
             if (may_inline && _should_inline(obj))
               {
                  if ((type == TYPE_IMG) ||
                      (type == TYPE_SCALE) ||
                      (type == TYPE_EDJE))
                    {
                       evas_object_smart_callback_call(obj, "popup", NULL);
                       handled = EINA_TRUE;
                    }
                  else if (type == TYPE_MOV)
                    {
                       evas_object_smart_callback_call(obj, "popup", NULL);
                       handled = EINA_TRUE;
                    }
               }
             if (!handled)
               {
                  if ((type == TYPE_IMG) ||
                      (type == TYPE_SCALE) ||
                      (type == TYPE_EDJE))
                    {
                       if ((config->helper.local.image) &&
                           (config->helper.local.image[0]))
                         cmd = config->helper.local.image;
                    }
                  else if (type == TYPE_MOV)
                    {
                       if ((config->helper.local.video) &&
                           (config->helper.local.video[0]))
                         cmd = config->helper.local.video;
                    }
                  else
                    {
                       if ((config->helper.local.general) &&
                           (config->helper.local.general[0]))
                         cmd = config->helper.local.general;
                    }
                  snprintf(buf, sizeof(buf), "%s %s", cmd, escaped);
                  free(escaped);
               }
          }
     }
   else if (url)
     {
        // remote file needs ecore-con-url
        cmd = "xdg-open";
        
        escaped = ecore_file_escape_name(s);
        if (escaped)
          {
             type = media_src_type_get(sd->link.string);
             if (may_inline && _should_inline(obj))
               {
                  if ((type == TYPE_IMG) ||
                      (type == TYPE_SCALE) ||
                      (type == TYPE_EDJE))
                    {
                       // XXX: begin fetch of url, once done, show
                       evas_object_smart_callback_call(obj, "popup", NULL);
                       handled = EINA_TRUE;
                    }
                  else if (type == TYPE_MOV)
                    {
                       // XXX: if no http:// add
                       evas_object_smart_callback_call(obj, "popup", NULL);
                       handled = EINA_TRUE;
                    }
               }
             if (!handled)
               {
                  if ((type == TYPE_IMG) ||
                      (type == TYPE_SCALE) ||
                      (type == TYPE_EDJE))
                    {
                       if ((config->helper.url.image) &&
                           (config->helper.url.image[0]))
                         cmd = config->helper.url.image;
                    }
                  else if (type == TYPE_MOV)
                    {
                       if ((config->helper.url.video) &&
                           (config->helper.url.video[0]))
                         cmd = config->helper.url.video;
                    }
                  else
                    {
                       if ((config->helper.url.general) &&
                           (config->helper.url.general[0]))
                         cmd = config->helper.url.general;
                    }
                  snprintf(buf, sizeof(buf), "%s %s", cmd, escaped);
                  free(escaped);
               }
          }
     }
   else
     {
        free(s);
        return;
     }
   free(s);
   if (!handled) ecore_exe_run(buf, NULL);
}

static void
_cb_ctxp_del(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED,
             void *event EINA_UNUSED)
{
   Termio *sd = data;
   EINA_SAFETY_ON_NULL_RETURN(sd);
   sd->link.ctxpopup = NULL;
   elm_object_focus_set(sd->self, EINA_TRUE);
}

static void
_cb_ctxp_dismissed(void *data EINA_UNUSED, Evas_Object *obj,
                   void *event EINA_UNUSED)
{
   evas_object_del(obj);
}

static void
_cb_ctxp_link_preview(void *data, Evas_Object *obj, void *event EINA_UNUSED)
{
   Evas_Object *term = data;
   _activate_link(term, EINA_TRUE);
   evas_object_del(obj);
}

static void
_cb_ctxp_link_open(void *data, Evas_Object *obj, void *event EINA_UNUSED)
{
   Evas_Object *term = data;
   _activate_link(term, EINA_FALSE);
   evas_object_del(obj);
}

static void
_cb_ctxp_link_copy(void *data, Evas_Object *obj, void *event EINA_UNUSED)
{
   Evas_Object *term = data;
   Termio *sd = evas_object_smart_data_get(term);
   EINA_SAFETY_ON_NULL_RETURN(sd);
   EINA_SAFETY_ON_NULL_RETURN(sd->link.string);
   _take_selection_text(term, ELM_SEL_TYPE_CLIPBOARD, sd->link.string);
   evas_object_del(obj);
}

static void
_cb_link_down(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event)
{
   Evas_Event_Mouse_Down *ev = event;
   Termio *sd = evas_object_smart_data_get(data);
   EINA_SAFETY_ON_NULL_RETURN(sd);
   
   if (ev->button == 1)
     {
        sd->link.down.down = EINA_TRUE;
        sd->link.down.x = ev->canvas.x;
        sd->link.down.y = ev->canvas.y;
     }
   else if (ev->button == 3)
     {
        Evas_Object *ctxp = elm_ctxpopup_add(sd->win);
        sd->link.ctxpopup = ctxp;

        if (sd->config->helper.inline_please)
          {
             int type = media_src_type_get(sd->link.string);

             if ((type == TYPE_IMG) ||
                 (type == TYPE_SCALE) ||
                 (type == TYPE_EDJE) ||
                 (type == TYPE_MOV))
               elm_ctxpopup_item_append(ctxp, "Preview", NULL,
                                        _cb_ctxp_link_preview, sd->self);
          }
        elm_ctxpopup_item_append(ctxp, "Open", NULL, _cb_ctxp_link_open,
                                 sd->self);
        elm_ctxpopup_item_append(ctxp, "Copy", NULL, _cb_ctxp_link_copy,
                                 sd->self);
        evas_object_move(ctxp, ev->canvas.x, ev->canvas.y);
        evas_object_show(ctxp);
        evas_object_smart_callback_add(ctxp, "dismissed",
                                       _cb_ctxp_dismissed, sd);
        evas_object_event_callback_add(ctxp, EVAS_CALLBACK_DEL,
                                       _cb_ctxp_del, sd);
     }
}

static Eina_Bool
_cb_link_up_delay(void *data)
{
   Termio *sd = evas_object_smart_data_get(data);
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, EINA_FALSE);
   
   sd->link_do_timer = NULL;
   if (!sd->didclick) _activate_link(data, EINA_TRUE);
   sd->didclick = EINA_FALSE;
   return EINA_FALSE;
}

static void
_cb_link_up(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event)
{
   Evas_Event_Mouse_Up *ev = event;
   Termio *sd = evas_object_smart_data_get(data);
   EINA_SAFETY_ON_NULL_RETURN(sd);

   if ((ev->button == 1) && (sd->link.down.down))
     {
        Evas_Coord dx, dy, finger_size;

        dx = abs(ev->canvas.x - sd->link.down.x);
        dy = abs(ev->canvas.y - sd->link.down.y);
        finger_size = elm_config_finger_size_get();

        if ((dx <= finger_size) && (dy <= finger_size))
          {
             if (sd->link_do_timer) ecore_timer_del(sd->link_do_timer);
             sd->link_do_timer = ecore_timer_add(0.2, _cb_link_up_delay, data);
          }
        sd->link.down.down = EINA_FALSE;
     }
}

#if !((ELM_VERSION_MAJOR == 1) && (ELM_VERSION_MINOR < 8))
static void
_cb_link_drag_move(void *data, Evas_Object *obj, Evas_Coord x, Evas_Coord y, Elm_Xdnd_Action action)
{
   const Evas_Modifier *em = NULL;
   Termio *sd = evas_object_smart_data_get(data);
   EINA_SAFETY_ON_NULL_RETURN(sd);

   printf("dnd %i %i act %i\n", x, y, action);
   em = evas_key_modifier_get(evas_object_evas_get(sd->event));
   if (em)
     {
        if (evas_key_modifier_is_set(em, "Control"))
          elm_drag_action_set(obj, ELM_XDND_ACTION_COPY);
        else
          elm_drag_action_set(obj, ELM_XDND_ACTION_MOVE);
     }
}

static void
_cb_link_drag_accept(void *data, Evas_Object *obj EINA_UNUSED, Eina_Bool doaccept)
{
   Termio *sd = evas_object_smart_data_get(data);
   EINA_SAFETY_ON_NULL_RETURN(sd);

   printf("dnd accept: %i\n", doaccept);
}

static void
_cb_link_drag_done(void *data, Evas_Object *obj EINA_UNUSED)
{
   Termio *sd = evas_object_smart_data_get(data);
   EINA_SAFETY_ON_NULL_RETURN(sd);

   printf("dnd done\n");
   sd->link.down.dnd = EINA_FALSE;
   if ((sd->link.down.dndobjdel) && (sd->link.down.dndobj))
     evas_object_del(sd->link.down.dndobj);
   sd->link.down.dndobj = NULL;
}

static Evas_Object *
_cb_link_icon_new(void *data, Evas_Object *par, Evas_Coord *xoff, Evas_Coord *yoff)
{
   Evas_Object *icon;
   Termio *sd = evas_object_smart_data_get(data);
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, NULL);

   icon = elm_button_add(par);
   elm_object_text_set(icon, sd->link.string);
   *xoff = 0;
   *yoff = 0;
   return icon;
}
#endif

static void
_cb_link_move(void *data, Evas *e EINA_UNUSED, Evas_Object *obj, void *event)
{
   Evas_Event_Mouse_Move *ev = event;
   Termio *sd = evas_object_smart_data_get(data);
   Evas_Coord dx, dy;
   EINA_SAFETY_ON_NULL_RETURN(sd);

   if (!sd->link.down.down) return;
   dx = abs(ev->cur.canvas.x - sd->link.down.x);
   dy = abs(ev->cur.canvas.y - sd->link.down.y);
   if ((sd->config->drag_links) &&
       (sd->link.string) &&
       ((dx > elm_config_finger_size_get()) ||
           (dy > elm_config_finger_size_get())))
     {
        sd->link.down.down = EINA_FALSE;
        sd->link.down.dnd = EINA_TRUE;
#if !((ELM_VERSION_MAJOR == 1) && (ELM_VERSION_MINOR < 8))
        printf("dnd start %s %i %i\n", sd->link.string,
               evas_key_modifier_is_set(ev->modifiers, "Control"),
               evas_key_modifier_is_set(ev->modifiers, "Shift"));
        if (evas_key_modifier_is_set(ev->modifiers, "Control"))
          elm_drag_start(obj, ELM_SEL_FORMAT_IMAGE, sd->link.string,
                         ELM_XDND_ACTION_COPY,
                         _cb_link_icon_new, data,
                         _cb_link_drag_move, data,
                         _cb_link_drag_accept, data,
                         _cb_link_drag_done, data);
        else
          elm_drag_start(obj, ELM_SEL_FORMAT_IMAGE, sd->link.string,
                         ELM_XDND_ACTION_MOVE,
                         _cb_link_icon_new, data,
                         _cb_link_drag_move, data,
                         _cb_link_drag_accept, data,
                         _cb_link_drag_done, data);
        sd->link.down.dndobj = obj;
        sd->link.down.dndobjdel = EINA_FALSE;
#endif
     }
}

static void
_update_link(Evas_Object *obj, Termio *sd,
             Eina_Bool same_link, Eina_Bool same_geom)
{
   EINA_SAFETY_ON_NULL_RETURN(sd);
   
   if (!same_link)
     {
        // check link and re-probe/fetch create popup preview
     }
   
   if (!same_geom)
     {
        Evas_Coord ox, oy, ow, oh;
        Evas_Object *o;
        // fix up edje objects "underlining" the link
        int y;

        evas_object_geometry_get(obj, &ox, &oy, &ow, &oh);
        if (!sd->link.suspend)
          {
             EINA_LIST_FREE(sd->link.objs, o)
               {
                  if (sd->link.down.dndobj == o)
                    {
                       sd->link.down.dndobjdel = EINA_TRUE;
                       evas_object_hide(o);
                    }
                  else
                    evas_object_del(o);
               }
             if (sd->link.string)
               {
                  if ((sd->link.string[0] == '/') || (link_is_url(sd->link.string)))
                    {
                       Evas_Coord _x = ox, _y = oy;
                       uint64_t xwin;

                       _x += sd->mouse.cx * sd->font.chw;
                       _y += sd->mouse.cy * sd->font.chh;
#if (ELM_VERSION_MAJOR > 1) || (ELM_VERSION_MINOR >= 8)
                       xwin = elm_win_window_id_get(sd->win);
# if (ELM_VERSION_MAJOR > 1) || ((ELM_VERSION_MAJOR == 1) && (ELM_VERSION_MINOR > 8)) // not a typo
                       if (strstr(ecore_evas_engine_name_get(ecore_evas_ecore_evas_get(evas_object_evas_get(sd->win))), "wayland"))
                         xwin = ((uint64_t)xwin << 32) + (uint64_t)getpid();
# endif
#else
                       xwin = elm_win_xwindow_get(sd->win);
#endif
                       ty_dbus_link_mousein(xwin, sd->link.string, _x, _y);
                    }
                  for (y = sd->link.y1; y <= sd->link.y2; y++)
                    {
                       o = edje_object_add(evas_object_evas_get(obj));
                       evas_object_smart_member_add(o, obj);
                       theme_apply(o, sd->config, "terminology/link");

                       if (y == sd->link.y1)
                         {
                            evas_object_move(o, ox + (sd->link.x1 * sd->font.chw),
                                             oy + (y * sd->font.chh));
                            if (sd->link.y1 == sd->link.y2)
                              evas_object_resize(o,
                                                 ((sd->link.x2 - sd->link.x1 + 1) * sd->font.chw),
                                                 sd->font.chh);
                            else
                              evas_object_resize(o,
                                                 ((sd->grid.w - sd->link.x1) * sd->font.chw),
                                                 sd->font.chh);
                         }
                       else if (y == sd->link.y2)
                         {
                            evas_object_move(o, ox, oy + (y * sd->font.chh));
                            evas_object_resize(o,
                                               ((sd->link.x2 + 1) * sd->font.chw),
                                               sd->font.chh);
                         }
                       else
                         {
                            evas_object_move(o, ox, oy + (y * sd->font.chh));
                            evas_object_resize(o, (sd->grid.w * sd->font.chw),
                                               sd->font.chh);
                         }

                       sd->link.objs = eina_list_append(sd->link.objs, o);
                       evas_object_show(o);
                       evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_DOWN,
                                                      _cb_link_down, obj);
                       evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_UP,
                                                      _cb_link_up, obj);
                       evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_MOVE,
                                                      _cb_link_move, obj);
                    }
               }
          }
     }
}

static void
_remove_links(Termio *sd, Evas_Object *obj)
{
   Eina_Bool same_link = EINA_FALSE, same_geom = EINA_FALSE;

   if (sd->link.string)
     {
        if ((sd->link.string[0] == '/') || (link_is_url(sd->link.string)))
          {
             Evas_Coord ox, oy;
             uint64_t xwin;

             evas_object_geometry_get(obj, &ox, &oy, NULL, NULL);

             ox += sd->mouse.cx * sd->font.chw;
             oy += sd->mouse.cy * sd->font.chh;
#if (ELM_VERSION_MAJOR > 1) || (ELM_VERSION_MINOR >= 8)
                       xwin = elm_win_window_id_get(sd->win);
# if (ELM_VERSION_MAJOR > 1) || ((ELM_VERSION_MAJOR == 1) && (ELM_VERSION_MINOR > 8)) // not a typo
                       if (strstr(ecore_evas_engine_name_get(ecore_evas_ecore_evas_get(evas_object_evas_get(sd->win))), "wayland"))
                         xwin = ((uint64_t)xwin << 32) + (uint64_t)getpid();
# endif
#else
                       xwin = elm_win_xwindow_get(sd->win);
#endif
             ty_dbus_link_mouseout(xwin, sd->link.string, ox, oy);
          }
        free(sd->link.string);
        sd->link.string = NULL;
     }
   sd->link.x1 = -1;
   sd->link.y1 = -1;
   sd->link.x2 = -1;
   sd->link.y2 = -1;
   sd->link.suspend = EINA_FALSE;
   _update_link(obj, sd, same_link, same_geom);
}

static void
_smart_mouseover_apply(Evas_Object *obj)
{
   char *s;
   int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
   Eina_Bool same_link = EINA_FALSE, same_geom = EINA_FALSE;
   Termio *sd = evas_object_smart_data_get(obj);

   EINA_SAFETY_ON_NULL_RETURN(sd);
   if ((sd->mouse.cx < 0) || (sd->mouse.cy < 0) ||
       (sd->link.suspend) || (!evas_object_focus_get(obj)))
     {
        _remove_links(sd, obj);
        return;
     }

   s = _termio_link_find(obj, sd->mouse.cx, sd->mouse.cy,
                         &x1, &y1, &x2, &y2);
   if (!s)
     {
        _remove_links(sd, obj);
        return;
     }

   if ((sd->link.string) && (!strcmp(sd->link.string, s)))
     same_link = EINA_TRUE;
   if (sd->link.string) free(sd->link.string);
   sd->link.string = s;

   if ((x1 == sd->link.x1) && (y1 == sd->link.y1) &&
       (x2 == sd->link.x2) && (y2 == sd->link.y2))
     same_geom = EINA_TRUE;
   if (((sd->link.suspend != 0) && (sd->link.objs)) ||
       ((sd->link.suspend == 0) && (!sd->link.objs)))
     same_geom = EINA_FALSE;
   sd->link.x1 = x1;
   sd->link.y1 = y1;
   sd->link.x2 = x2;
   sd->link.y2 = y2;
   _update_link(obj, sd, same_link, same_geom);
}

static Eina_Bool
_smart_mouseover_delay(void *data)
{
   Termio *sd = evas_object_smart_data_get(data);
   
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, EINA_FALSE);
   sd->mouseover_delay = NULL;
   _smart_mouseover_apply(data);
   return EINA_FALSE;
}

#define INT_SWAP(_a, _b) do {    \
    int _swap = _a; _a = _b; _b = _swap; \
} while (0)

static void
_smart_media_clicked(void *data, Evas_Object *obj, void *info EINA_UNUSED)
{
//   Termio *sd = evas_object_smart_data_get(data);
   Termblock *blk;
   const char *file = media_get(obj);
   if (!file) return;
   blk = evas_object_data_get(obj, "blk");
   if (blk)
     {
        if (blk->link)
          {
             int type = media_src_type_get(blk->link);
             Config *config = termio_config_get(data);
             
             if (config)
               {
                  if ((!config->helper.inline_please) ||
                      (!((type == TYPE_IMG) || (type == TYPE_SCALE) ||
                         (type == TYPE_EDJE) || (type == TYPE_MOV))))
                    {
                       const char *cmd = NULL;
                       
                       file = blk->link;
                       if ((config->helper.local.general) &&
                           (config->helper.local.general[0]))
                         cmd = config->helper.local.general;
                       if (cmd)
                         {
                            char buf[PATH_MAX];
                            
                            snprintf(buf, sizeof(buf), "%s %s", cmd, file);
                            ecore_exe_run(buf, NULL);
                            return;
                         }
                    }
                  file = blk->link;
               }
          }
     }
   evas_object_smart_callback_call(data, "popup", (void *)file);
}

static void
_smart_media_del(void *data, Evas *e EINA_UNUSED, Evas_Object *obj, void *info EINA_UNUSED)
{
   Termblock *blk = data;
   
   if (blk->obj == obj)
     {
        evas_object_event_callback_del_full
          (blk->obj, EVAS_CALLBACK_DEL,
              _smart_media_del, blk);
        blk->obj = NULL;
     }
}

static void
_block_edje_signal_cb(void *data, Evas_Object *obj EINA_UNUSED, const char *sig, const char *src)
{
   Termblock *blk = data;
   Termio *sd = evas_object_smart_data_get(blk->pty->obj);
   char *buf = NULL, *chid = NULL;
   int buflen = 0;
   Eina_List *l;

   EINA_SAFETY_ON_NULL_RETURN(sd);
   if ((!blk->chid) || (!sd->cur_chids)) return;
   EINA_LIST_FOREACH(sd->cur_chids, l, chid)
     {
        if (!(!strcmp(blk->chid, chid))) break;
        chid = NULL;
     }
   if (!chid) return;
   if ((!strcmp(sig, "drag")) ||
       (!strcmp(sig, "drag,start")) ||
       (!strcmp(sig, "drag,stop")) ||
       (!strcmp(sig, "drag,step")) ||
       (!strcmp(sig, "drag,set")))
     {
        int v1, v2;
        double f1 = 0.0, f2 = 0.0;
        
        edje_object_part_drag_value_get(blk->obj, src, &f1, &f2);
        v1 = (int)(f1 * 1000.0);
        v2 = (int)(f2 * 1000.0);
        buf = alloca(strlen(src) + strlen(blk->chid) + 256);
        buflen = sprintf(buf, "%c};%s\n%s\n%s\n%i\n%i", 0x1b,
                         blk->chid, sig, src, v1, v2);
        termpty_write(sd->pty, buf, buflen + 1);
     }
   else
     {
        buf = alloca(strlen(sig) + strlen(src) + strlen(blk->chid) + 128);
        buflen = sprintf(buf, "%c}signal;%s\n%s\n%s", 0x1b,
                         blk->chid, sig, src);
        termpty_write(sd->pty, buf, buflen + 1);
     }
}

static void
_block_edje_message_cb(void *data, Evas_Object *obj EINA_UNUSED, Edje_Message_Type type, int id, void *msg)
{
   Termblock *blk = data;
   Termio *sd = evas_object_smart_data_get(blk->pty->obj);
   char *chid = NULL, buf[4096];
   Eina_List *l;
   int buflen;

   EINA_SAFETY_ON_NULL_RETURN(sd);
   if ((!blk->chid) || (!sd->cur_chids)) return;
   EINA_LIST_FOREACH(sd->cur_chids, l, chid)
     {
        if (!(!strcmp(blk->chid, chid))) break;
        chid = NULL;
     }
   if (!chid) return;
   switch (type)
     {
      case EDJE_MESSAGE_STRING:
          {
             Edje_Message_String *m = msg;
             
             buflen = sprintf(buf, "%c}message;%s\n%i\nstring\n%s", 0x1b,
                              blk->chid, id, m->str);
             termpty_write(sd->pty, buf, buflen + 1);
          }
        break;
      case EDJE_MESSAGE_INT:
          {
             Edje_Message_Int *m = msg;
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nint\n%i", 0x1b,
                               blk->chid, id, m->val);
             termpty_write(sd->pty, buf, buflen + 1);
          }
        break;
      case EDJE_MESSAGE_FLOAT:
          {
             Edje_Message_Float *m = msg;
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nfloat\n%i", 0x1b,
                               blk->chid, id, (int)(m->val * 1000.0));
             termpty_write(sd->pty, buf, buflen + 1);
          }
        break;
      case EDJE_MESSAGE_STRING_SET:
          {
             Edje_Message_String_Set *m = msg;
             int i;
             char zero[1] = { 0 };
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nstring_set\n%i", 0x1b,
                               blk->chid, id, m->count);
             termpty_write(sd->pty, buf, buflen);
             for (i = 0; i < m->count; i++)
               {
                  termpty_write(sd->pty, "\n", 1);
                  termpty_write(sd->pty, m->str[i], strlen(m->str[i]));
               }
             termpty_write(sd->pty, zero, 1);
         }
        break;
      case EDJE_MESSAGE_INT_SET:
          {
             Edje_Message_Int_Set *m = msg;
             int i;
             char zero[1] = { 0 };
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nint_set\n%i", 0x1b,
                               blk->chid, id, m->count);
             termpty_write(sd->pty, buf, buflen);
             for (i = 0; i < m->count; i++)
               {
                  termpty_write(sd->pty, "\n", 1);
                  buflen = snprintf(buf, sizeof(buf), "%i", m->val[i]);
                  termpty_write(sd->pty, buf, buflen);
               }
             termpty_write(sd->pty, zero, 1);
          }
        break;
      case EDJE_MESSAGE_FLOAT_SET:
          {
             Edje_Message_Float_Set *m = msg;
             int i;
             char zero[1] = { 0 };
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nfloat_set\n%i", 0x1b,
                               blk->chid, id, m->count);
             termpty_write(sd->pty, buf, buflen);
             for (i = 0; i < m->count; i++)
               {
                  termpty_write(sd->pty, "\n", 1);
                  buflen = snprintf(buf, sizeof(buf), "%i", (int)(m->val[i] * 1000.0));
                  termpty_write(sd->pty, buf, buflen);
               }
             termpty_write(sd->pty, zero, 1);
          }
        break;
      case EDJE_MESSAGE_STRING_INT:
          {
             Edje_Message_String_Int *m = msg;
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nstring_int\n%s\n%i", 0x1b,
                               blk->chid, id, m->str, m->val);
             termpty_write(sd->pty, buf, buflen + 1);
          }
        break;
      case EDJE_MESSAGE_STRING_FLOAT:
          {
             Edje_Message_String_Float *m = msg;
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nstring_float\n%s\n%i", 0x1b,
                               blk->chid, id, m->str, (int)(m->val * 1000.0));
             termpty_write(sd->pty, buf, buflen + 1);
          }
        break;
      case EDJE_MESSAGE_STRING_INT_SET:
          {
             Edje_Message_String_Int_Set *m = msg;
             int i;
             char zero[1] = { 0 };
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nstring_int_set\n%i", 0x1b,
                               blk->chid, id, m->count);
             termpty_write(sd->pty, buf, buflen);
             termpty_write(sd->pty, "\n", 1);
             termpty_write(sd->pty, m->str, strlen(m->str));
             for (i = 0; i < m->count; i++)
               {
                  termpty_write(sd->pty, "\n", 1);
                  buflen = snprintf(buf, sizeof(buf), "%i", m->val[i]);
                  termpty_write(sd->pty, buf, buflen);
               }
             termpty_write(sd->pty, zero, 1);
          }
        break;
      case EDJE_MESSAGE_STRING_FLOAT_SET:
          {
             Edje_Message_String_Float_Set *m = msg;
             int i;
             char zero[1] = { 0 };
             
             buflen = snprintf(buf, sizeof(buf),
                               "%c}message;%s\n%i\nstring_float_set\n%i", 0x1b,
                               blk->chid, id, m->count);
             termpty_write(sd->pty, buf, buflen);
             termpty_write(sd->pty, "\n", 1);
             termpty_write(sd->pty, m->str, strlen(m->str));
             for (i = 0; i < m->count; i++)
               {
                  termpty_write(sd->pty, "\n", 1);
                  buflen = snprintf(buf, sizeof(buf), "%i", (int)(m->val[i] * 1000.0));
                  termpty_write(sd->pty, buf, buflen);
               }
             termpty_write(sd->pty, zero, 1);
          }
        break;
      default:
        break;
     }
}

static void
_block_edje_cmds(Termpty *ty, Termblock *blk, Eina_List *cmds, Eina_Bool created)
{
   Eina_List *l;
   char *s;
        
#define ISCMD(cmd) !strcmp(s, cmd)
#define GETS(var) l = l->next; if (!l) return; var = l->data
#define GETI(var) l = l->next; if (!l) return; var = atoi(l->data)
#define GETF(var) l = l->next; if (!l) return; var = (double)atoi(l->data) / 1000.0
   l = cmds;
   while (l)
     {
        s = l->data;
        
        /////////////////////////////////////////////////////////////////////
        if (ISCMD("text")) // set text part
          {
             char *prt, *txt;
             
             GETS(prt);
             GETS(txt);
             edje_object_part_text_set(blk->obj, prt, txt);
          }
        /////////////////////////////////////////////////////////////////////
        else if (ISCMD("emit")) // emit signal
          {
             char *sig, *src;
             
             GETS(sig);
             GETS(src);
             edje_object_signal_emit(blk->obj, sig, src);
          }
        /////////////////////////////////////////////////////////////////////
        else if (ISCMD("drag")) // set dragable
          {
             char *prt, *val;
             double v1, v2;
             
             GETS(prt);
             GETS(val);
             GETF(v1);
             GETF(v2);
             if (!strcmp(val, "value"))
               edje_object_part_drag_value_set(blk->obj, prt, v1, v2);
             else if (!strcmp(val, "size"))
               edje_object_part_drag_size_set(blk->obj, prt, v1, v2);
             else if (!strcmp(val, "step"))
               edje_object_part_drag_step_set(blk->obj, prt, v1, v2);
             else if (!strcmp(val, "page"))
               edje_object_part_drag_page_set(blk->obj, prt, v1, v2);
          }
        /////////////////////////////////////////////////////////////////////
        else if (ISCMD("message")) // send message
          {
             int id;
             char *typ;
             
             GETI(id);
             GETS(typ);
             if (!strcmp(typ, "string"))
               {
                  Edje_Message_String *m;
                  
                  m = alloca(sizeof(Edje_Message_String));
                  GETS(m->str);
                  edje_object_message_send(blk->obj, EDJE_MESSAGE_STRING,
                                           id, m);
               }
             else if (!strcmp(typ, "int"))
               {
                  Edje_Message_Int *m;
                  
                  m = alloca(sizeof(Edje_Message_Int));
                  GETI(m->val);
                  edje_object_message_send(blk->obj, EDJE_MESSAGE_INT,
                                           id, m);
               }
             else if (!strcmp(typ, "float"))
               {
                  Edje_Message_Float *m;
                  
                  m = alloca(sizeof(Edje_Message_Float));
                  GETF(m->val);
                  edje_object_message_send(blk->obj, EDJE_MESSAGE_FLOAT,
                                           id, m);
               }
             else if (!strcmp(typ, "string_set"))
               {
                  Edje_Message_String_Set *m;
                  int i, count;
                  
                  GETI(count);
                  m = alloca(sizeof(Edje_Message_String_Set) + 
                             ((count - 1) * sizeof(char *)));
                  m->count = count;
                  for (i = 0; i < m->count; i++)
                    {
                       GETS(m->str[i]);
                    }
                  edje_object_message_send(blk->obj,
                                           EDJE_MESSAGE_STRING_SET,
                                           id, m);
               }
             else if (!strcmp(typ, "int_set"))
               {
                  Edje_Message_Int_Set *m;
                  int i, count;
                  
                  GETI(count);
                  m = alloca(sizeof(Edje_Message_Int_Set) + 
                             ((count - 1) * sizeof(int)));
                  m->count = count;
                  for (i = 0; i < m->count; i++)
                    {
                       GETI(m->val[i]);
                    }
                  edje_object_message_send(blk->obj,
                                           EDJE_MESSAGE_INT_SET,
                                           id, m);
               }
             else if (!strcmp(typ, "float_set"))
               {
                  Edje_Message_Float_Set *m;
                  int i, count;
                  
                  GETI(count);
                  m = alloca(sizeof(Edje_Message_Float_Set) +
                             ((count - 1) * sizeof(double)));
                  m->count = count;
                  for (i = 0; i < m->count; i++)
                    {
                       GETF(m->val[i]);
                    }
                  edje_object_message_send(blk->obj,
                                           EDJE_MESSAGE_FLOAT_SET,
                                           id, m);
               }
             else if (!strcmp(typ, "string_int"))
               {
                  Edje_Message_String_Int *m;
                  
                  m = alloca(sizeof(Edje_Message_String_Int));
                  GETS(m->str);
                  GETI(m->val);
                  edje_object_message_send(blk->obj, EDJE_MESSAGE_STRING_INT,
                                           id, m);
               }
             else if (!strcmp(typ, "string_float"))
               {
                  Edje_Message_String_Float *m;
                  
                  m = alloca(sizeof(Edje_Message_String_Float));
                  GETS(m->str);
                  GETF(m->val);
                  edje_object_message_send(blk->obj, EDJE_MESSAGE_STRING_FLOAT,
                                           id, m);
               }
             else if (!strcmp(typ, "string_int_set"))
               {
                  Edje_Message_String_Int_Set *m;
                  int i, count;
                  
                  GETI(count);
                  m = alloca(sizeof(Edje_Message_String_Int_Set) + 
                             ((count - 1) * sizeof(int)));
                  GETS(m->str);
                  m->count = count;
                  for (i = 0; i < m->count; i++)
                    {
                       GETI(m->val[i]);
                    }
                  edje_object_message_send(blk->obj,
                                           EDJE_MESSAGE_STRING_INT_SET,
                                           id, m);
               }
             else if (!strcmp(typ, "string_float_set"))
               {
                  Edje_Message_String_Float_Set *m;
                  int i, count;
                  
                  GETI(count);
                  m = alloca(sizeof(Edje_Message_String_Float_Set) + 
                             ((count - 1) * sizeof(double)));
                  GETS(m->str);
                  m->count = count;
                  for (i = 0; i < m->count; i++)
                    {
                       GETF(m->val[i]);
                    }
                  edje_object_message_send(blk->obj,
                                           EDJE_MESSAGE_STRING_FLOAT_SET,
                                           id, m);
               }
          }
        /////////////////////////////////////////////////////////////////////
        else if (ISCMD("chid")) // set callback channel id
          {
             char *chid;
             
             GETS(chid);
             if (!blk->chid)
               {
                  blk->chid = eina_stringshare_add(chid);
                  termpty_block_chid_update(ty, blk);
               }
             if (created)
               {
                  edje_object_signal_callback_add(blk->obj, "*", "*",
                                                  _block_edje_signal_cb,
                                                  blk);
                  edje_object_message_handler_set(blk->obj,
                                                  _block_edje_message_cb,
                                                  blk);
               }
          }
        if (l) l = l->next;
     }
}

static void
_block_edje_activate(Evas_Object *obj, Termblock *blk)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Eina_Bool ok = EINA_FALSE;

   EINA_SAFETY_ON_NULL_RETURN(sd);
   if ((!blk->path) || (!blk->link)) return;
   blk->obj = edje_object_add(evas_object_evas_get(obj));
   if (blk->path[0] == '/')
     ok = edje_object_file_set(blk->obj, blk->path, blk->link);
   else if (!strcmp(blk->path, "THEME"))
     ok = edje_object_file_set(blk->obj, 
                               config_theme_path_default_get
                               (sd->config),
                               blk->link);
   else
     {
        char path[PATH_MAX], home[PATH_MAX];
        
        if (homedir_get(home, sizeof(home)))
          {
             snprintf(path, sizeof(path), "%s/.terminology/objlib/%s",
                      home, blk->path);
             ok = edje_object_file_set(blk->obj, path, blk->link);
          }
        if (!ok)
          {
             snprintf(path, sizeof(path), "%s/objlib/%s",
                      elm_app_data_dir_get(), blk->path);
             ok = edje_object_file_set(blk->obj, path, blk->link);
          }
     }
   evas_object_smart_member_add(blk->obj, obj);
   evas_object_stack_above(blk->obj, sd->event);
   evas_object_show(blk->obj);
   evas_object_data_set(blk->obj, "blk", blk);

   if (ok) _block_edje_cmds(sd->pty, blk, blk->cmds, EINA_TRUE);
}

static void
_block_media_activate(Evas_Object *obj, Termblock *blk)
{
   Termio *sd = evas_object_smart_data_get(obj);
   int type = 0;
   int media = MEDIA_STRETCH;

   EINA_SAFETY_ON_NULL_RETURN(sd);
   if (blk->scale_stretch) media = MEDIA_STRETCH;
   else if (blk->scale_center) media = MEDIA_POP;
   else if (blk->scale_fill) media = MEDIA_BG;
   else if (blk->thumb) media = MEDIA_THUMB;
//   media = MEDIA_POP;
   if (!blk->was_active_before) media |= MEDIA_SAVE;
   else media |= MEDIA_RECOVER | MEDIA_SAVE;
   blk->obj = media_add(obj, blk->path, sd->config, media, &type);
   evas_object_event_callback_add
     (blk->obj, EVAS_CALLBACK_DEL, _smart_media_del, blk);
   blk->type = type;
   evas_object_smart_member_add(blk->obj, obj);
   evas_object_stack_above(blk->obj, sd->grid.obj);
   evas_object_show(blk->obj);
   evas_object_data_set(blk->obj, "blk", blk);
   if (blk->thumb)
     evas_object_smart_callback_add
     (blk->obj, "clicked", _smart_media_clicked, obj);
}

static void
_block_activate(Evas_Object *obj, Termblock *blk)
{
   Termio *sd = evas_object_smart_data_get(obj);

   EINA_SAFETY_ON_NULL_RETURN(sd);
   if (blk->active) return;
   blk->active = EINA_TRUE;
   if (blk->obj) return;
   if (blk->edje) _block_edje_activate(obj, blk);
   else _block_media_activate(obj, blk);
   blk->was_active_before = EINA_TRUE;
   if (!blk->was_active)
     sd->pty->block.active = eina_list_append(sd->pty->block.active, blk);
}

static void
_smart_apply(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Coord ox, oy, ow, oh;
   Eina_List *l, *ln;
   Termblock *blk;
   int x, y, w, ch1 = 0, ch2 = 0, inv = 0;

   EINA_SAFETY_ON_NULL_RETURN(sd);
   evas_object_geometry_get(obj, &ox, &oy, &ow, &oh);
   
   EINA_LIST_FOREACH(sd->pty->block.active, l, blk)
     {
        blk->was_active = blk->active;
        blk->active = EINA_FALSE;
     }
   inv = sd->pty->state.reverse;
   termpty_cellcomp_freeze(sd->pty);
   for (y = 0; y < sd->grid.h; y++)
     {
        Termcell *cells;
        Evas_Textgrid_Cell *tc;

        w = 0;
        cells = termpty_cellrow_get(sd->pty, y - sd->scroll, &w);
        tc = evas_object_textgrid_cellrow_get(sd->grid.obj, y);
        if (!tc) continue;
        ch1 = -1;
        for (x = 0; x < sd->grid.w; x++)
          {
             if ((!cells) || (x >= w))
               {
                  if ((tc[x].codepoint != 0) ||
                      (tc[x].bg != COL_INVIS) ||
                      (tc[x].bg_extended))
                    {
                       if (ch1 < 0) ch1 = x;
                       ch2 = x;
                    }
                  tc[x].codepoint = 0;
                  if (inv) tc[x].bg = COL_INVERSEBG;
                  else tc[x].bg = COL_INVIS;
                  tc[x].bg_extended = 0;
                  tc[x].double_width = 0;
                  tc[x].underline = 0;
                  tc[x].strikethrough = 0;
               }
             else
               {
                  int bid, bx = 0, by = 0;
                  
                  bid = termpty_block_id_get(&(cells[x]), &bx, &by);
                  if (bid >= 0)
                    {
                       if (ch1 < 0) ch1 = x;
                       ch2 = x;
                       tc[x].codepoint = 0;
                       tc[x].fg_extended = 0;
                       tc[x].bg_extended = 0;
                       tc[x].underline = 0;
                       tc[x].strikethrough = 0;
                       tc[x].fg = COL_INVIS;
                       tc[x].bg = COL_INVIS;
#if defined(SUPPORT_DBLWIDTH)
                       tc[x].double_width = 0;
#endif
                       blk = termpty_block_get(sd->pty, bid);
                       if (blk)
                         {
                            _block_activate(obj, blk);
                            blk->x = (x - bx);
                            blk->y = (y - by);
                            evas_object_move(blk->obj,
                                             ox + (blk->x * sd->font.chw),
                                             oy + (blk->y * sd->font.chh));
                            evas_object_resize(blk->obj,
                                               blk->w * sd->font.chw,
                                               blk->h * sd->font.chh);
                         }
                    }
                  else if (cells[x].att.invisible)
                    {
                       if ((tc[x].codepoint != 0) ||
                           (tc[x].bg != COL_INVIS) ||
                           (tc[x].bg_extended))
                         {
                            if (ch1 < 0) ch1 = x;
                            ch2 = x;
                         }
                       tc[x].codepoint = 0;
                       if (inv) tc[x].bg = COL_INVERSEBG;
                       else tc[x].bg = COL_INVIS;
                       tc[x].bg_extended = 0;
                       tc[x].underline = 0;
                       tc[x].strikethrough = 0;
#if defined(SUPPORT_DBLWIDTH)
                       tc[x].double_width = cells[x].att.dblwidth;
#endif
                       if ((tc[x].double_width) && (tc[x].codepoint == 0) &&
                           (ch2 == x - 1))
                         ch2 = x;
                    }
                  else
                    {
                       int fg, bg, fgext, bgext, codepoint;

                       // colors
                       fg = cells[x].att.fg;
                       bg = cells[x].att.bg;
                       fgext = cells[x].att.fg256;
                       bgext = cells[x].att.bg256;
                       codepoint = cells[x].codepoint;

                       if ((fg == COL_DEF) && (cells[x].att.inverse ^ inv))
                         fg = COL_INVERSEBG;
                       if (bg == COL_DEF)
                         {
                            if (cells[x].att.inverse ^ inv)
                              bg = COL_INVERSE;
                            else if (!bgext)
                              bg = COL_INVIS;
                         }
                       if ((cells[x].att.fgintense) && (!fgext)) fg += 48;
                       if ((cells[x].att.bgintense) && (!bgext)) bg += 48;
                       if (cells[x].att.inverse ^ inv)
                         {
                            int t;
                            t = fgext; fgext = bgext; bgext = t;
                            t = fg; fg = bg; bg = t;
                         }
                       if ((cells[x].att.bold) && (!fgext)) fg += 12;
                       if ((cells[x].att.faint) && (!fgext)) fg += 24;
                       if ((tc[x].codepoint != codepoint) ||
                           (tc[x].fg != fg) ||
                           (tc[x].bg != bg) ||
                           (tc[x].fg_extended != fgext) ||
                           (tc[x].bg_extended != bgext) ||
                           (tc[x].underline != cells[x].att.underline) ||
                           (tc[x].strikethrough != cells[x].att.strike) ||
                           (sd->debugwhite))
                         {
                            if (ch1 < 0) ch1 = x;
                            ch2 = x;
                         }
                       tc[x].fg_extended = fgext;
                       tc[x].bg_extended = bgext;
                       if (sd->debugwhite)
                         {
                            if (cells[x].att.newline)
                              tc[x].strikethrough = 1;
                            else
                              tc[x].strikethrough = 0;
                            if (cells[x].att.autowrapped)
                              tc[x].underline = 1;
                            else
                              tc[x].underline = 0;
//                            if (cells[x].att.tab)
//                              tc[x].underline = 1;
//                            else
//                              tc[x].underline = 0;
                            if ((cells[x].att.newline) ||
                                (cells[x].att.autowrapped))
                              {
                                 fg = 8;
                                 bg = 4;
                                 codepoint = '!';
                              }
                         }
                       else
                         {
                            tc[x].underline = cells[x].att.underline;
                            tc[x].strikethrough = cells[x].att.strike;
                         }
                       tc[x].fg = fg;
                       tc[x].bg = bg;
                       tc[x].codepoint = codepoint;
#if defined(SUPPORT_DBLWIDTH)
                       tc[x].double_width = cells[x].att.dblwidth;
#endif
                       if ((tc[x].double_width) && (tc[x].codepoint == 0) &&
                           (ch2 == x - 1))
                         ch2 = x;
                       // cells[x].att.italic // never going 2 support
                       // cells[x].att.blink
                       // cells[x].att.blink2
                    }
               }
          }
        evas_object_textgrid_cellrow_set(sd->grid.obj, y, tc);
        /* only bothering to keep 1 change span per row - not worth doing
         * more really */
        if (ch1 >= 0)
          evas_object_textgrid_update_add(sd->grid.obj, ch1, y,
                                          ch2 - ch1 + 1, 1);
     }
   termpty_cellcomp_thaw(sd->pty);
   
   EINA_LIST_FOREACH_SAFE(sd->pty->block.active, l, ln, blk)
     {
        if (!blk->active)
          {
             blk->was_active = EINA_FALSE;
             // XXX: move to func
             if (blk->obj)
               {
                  // XXX: handle if edje not media
                  evas_object_event_callback_del_full
                    (blk->obj, EVAS_CALLBACK_DEL,
                        _smart_media_del, blk);
                  evas_object_del(blk->obj);
                  blk->obj = NULL;
               }
             sd->pty->block.active = eina_list_remove_list
               (sd->pty->block.active, l);
          }
     }
   
   if ((sd->scroll != 0) || (sd->pty->state.hidecursor))
     evas_object_hide(sd->cursor.obj);
   else
     evas_object_show(sd->cursor.obj);
   sd->cursor.x = sd->pty->state.cx;
   sd->cursor.y = sd->pty->state.cy;
   evas_object_move(sd->cursor.obj,
                    ox + (sd->cursor.x * sd->font.chw),
                    oy + (sd->cursor.y * sd->font.chh));
   if (sd->pty->selection.is_active)
     {
        int start_x, start_y, end_x, end_y;
        int size_top, size_bottom;

        start_x = sd->pty->selection.start.x;
        start_y = sd->pty->selection.start.y;
        end_x   = sd->pty->selection.end.x;
        end_y   = sd->pty->selection.end.y;

        if (sd->pty->selection.is_box)
          {
             if (start_y > end_y)
               INT_SWAP(start_y, end_y);
             if (start_x > end_x)
               INT_SWAP(start_x, end_x);
           }
         else
           {
              if ((start_y > end_y) ||
                  ((start_y == end_y) && (end_x < start_x)))
                {
                   INT_SWAP(start_y, end_y);
                   INT_SWAP(start_x, end_x);
                }
           }
        size_top = start_x * sd->font.chw;

        size_bottom = (sd->grid.w - end_x - 1) * sd->font.chw;

        evas_object_size_hint_min_set(sd->sel.top,
                                      size_top,
                                      sd->font.chh);
        evas_object_size_hint_max_set(sd->sel.top,
                                      size_top,
                                      sd->font.chh);
        evas_object_size_hint_min_set(sd->sel.bottom,
                                      size_bottom,
                                      sd->font.chh);
        evas_object_size_hint_max_set(sd->sel.bottom,
                                      size_bottom,
                                      sd->font.chh);
        evas_object_move(sd->sel.theme,
                         ox,
                         oy + ((start_y + sd->scroll) * sd->font.chh));
        evas_object_resize(sd->sel.theme,
                           sd->grid.w * sd->font.chw,
                           (end_y + 1 - start_y) * sd->font.chh);

        if (sd->pty->selection.is_box)
          {
             edje_object_signal_emit(sd->sel.theme,
                                  "mode,oneline", "terminology");
          }
        else
          {
             if ((start_y == end_y) ||
                 ((start_x == 0) && (end_x == (sd->grid.w - 1))))
               {
                  edje_object_signal_emit(sd->sel.theme,
                                          "mode,oneline", "terminology");
               }
             else if ((start_y == (end_y - 1)) &&
                      (start_x > end_x))
               {
                  edje_object_signal_emit(sd->sel.theme,
                                          "mode,disjoint", "terminology");
               }
             else if (start_x == 0)
               {
                  edje_object_signal_emit(sd->sel.theme,
                                          "mode,topfull", "terminology");
               }
             else if (end_x == (sd->grid.w - 1))
               {
                  edje_object_signal_emit(sd->sel.theme,
                                          "mode,bottomfull", "terminology");
               }
             else
               {
                  edje_object_signal_emit(sd->sel.theme,
                                          "mode,multiline", "terminology");
               }
          }
        evas_object_show(sd->sel.theme);
     }
   else
     evas_object_hide(sd->sel.theme);
   if (sd->mouseover_delay) ecore_timer_del(sd->mouseover_delay);
   sd->mouseover_delay = ecore_timer_add(0.05, _smart_mouseover_delay, obj);
}

static void
_smart_size(Evas_Object *obj, int w, int h, Eina_Bool force)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN(sd);

   if (w < 1) w = 1;
   if (h < 1) h = 1;
   if (!force)
     {
        if ((w == sd->grid.w) && (h == sd->grid.h)) return;
     }

   evas_event_freeze(evas_object_evas_get(obj));
   evas_object_textgrid_size_set(sd->grid.obj, w, h);
   sd->grid.w = w;
   sd->grid.h = h;
   evas_object_resize(sd->cursor.obj, sd->font.chw, sd->font.chh);
   evas_object_size_hint_min_set(obj, sd->font.chw, sd->font.chh);
   if (!sd->noreqsize)
     evas_object_size_hint_request_set(obj,
                                       sd->font.chw * sd->grid.w,
                                       sd->font.chh * sd->grid.h);
   _sel_set(obj, EINA_FALSE);
   termpty_resize(sd->pty, w, h);
   _smart_calculate(obj);
   _smart_apply(obj);
   evas_event_thaw(evas_object_evas_get(obj));
}

static Eina_Bool
_smart_cb_delayed_size(void *data)
{
   Evas_Object *obj = data;
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Coord ow = 0, oh = 0;
   int w, h;

   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, EINA_FALSE);
   sd->delayed_size_timer = NULL;

   evas_object_geometry_get(obj, NULL, NULL, &ow, &oh);

   w = ow / sd->font.chw;
   h = oh / sd->font.chh;
   _smart_size(obj, w, h, EINA_FALSE);
   return EINA_FALSE;
}

static Eina_Bool
_smart_cb_change(void *data)
{
   Evas_Object *obj = data;
   Termio *sd = evas_object_smart_data_get(obj);

   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, EINA_FALSE);
   sd->anim = NULL;
   _smart_apply(obj);
   evas_object_smart_callback_call(obj, "changed", NULL);
   return EINA_FALSE;
}

static void
_smart_update_queue(Evas_Object *obj, Termio *sd)
{
   if (sd->anim) return;
   sd->anim = ecore_animator_add(_smart_cb_change, obj);
}

static void
_lost_selection_reset_job(void *data)
{
   Termio *sd = evas_object_smart_data_get(data);
   EINA_SAFETY_ON_NULL_RETURN(sd);

   sd->sel_reset_job = NULL;
   elm_cnp_selection_set(sd->win, sd->sel_type,
                         ELM_SEL_FORMAT_TEXT,
                         sd->sel_str, strlen(sd->sel_str));
   elm_cnp_selection_loss_callback_set(sd->win, sd->sel_type,
                                       _lost_selection, data);
}

static void
_lost_selection(void *data, Elm_Sel_Type selection)
{
   Eina_List *l;
   Evas_Object *obj;
   double t = ecore_time_get();
   EINA_LIST_FOREACH(terms, l, obj)
     {
        Termio *sd = evas_object_smart_data_get(obj);
        if (!sd) continue;
        if ((t - sd->set_sel_at) < 0.2) /// hack
          {
             if ((sd->have_sel) && (sd->sel_str) && (!sd->reset_sel))
               {
                  sd->reset_sel = EINA_TRUE;
                  if (sd->sel_reset_job) ecore_job_del(sd->sel_reset_job);
                  sd->sel_reset_job = ecore_job_add
                    (_lost_selection_reset_job, data);
               }
             continue;
          }
        if (sd->have_sel)
          {
             if (sd->sel_str)
               {
                  eina_stringshare_del(sd->sel_str);
                  sd->sel_str = NULL;
               }
             _sel_set(obj, EINA_FALSE);
             elm_object_cnp_selection_clear(sd->win, selection);
             _smart_update_queue(obj, sd);
             sd->have_sel = EINA_FALSE;
          }
     }
}

static void
_take_selection_text(Evas_Object *obj, Elm_Sel_Type type, const char *text)
{
   Termio *sd = evas_object_smart_data_get(obj);

   EINA_SAFETY_ON_NULL_RETURN(sd);

   text = eina_stringshare_add(text);

   sd->have_sel = EINA_FALSE;
   sd->reset_sel = EINA_FALSE;
   sd->set_sel_at = ecore_time_get(); // hack
   sd->sel_type = type;
   elm_cnp_selection_set(sd->win, type,
                         ELM_SEL_FORMAT_TEXT,
                         text,
                         eina_stringshare_strlen(text));
   elm_cnp_selection_loss_callback_set(sd->win, type,
                                       _lost_selection, obj);
   sd->have_sel = EINA_TRUE;
   if (sd->sel_str) eina_stringshare_del(sd->sel_str);
   sd->sel_str = text;
}

static void
_take_selection(Evas_Object *obj, Elm_Sel_Type type)
{
   Termio *sd = evas_object_smart_data_get(obj);
   int start_x = 0, start_y = 0, end_x = 0, end_y = 0;
   char *s = NULL;
   size_t len = 0;

   EINA_SAFETY_ON_NULL_RETURN(sd);
   if (sd->pty->selection.is_active)
     {
        start_x = sd->pty->selection.start.x;
        start_y = sd->pty->selection.start.y;
        end_x = sd->pty->selection.end.x;
        end_y = sd->pty->selection.end.y;
     }

   if (sd->pty->selection.is_box)
     {
        int i;
        Eina_Strbuf *sb;

        if (start_y > end_y)
          INT_SWAP(start_y, end_y);
        if (start_x > end_x)
          INT_SWAP(start_x, end_x);

        sb = eina_strbuf_new();
        for (i = start_y; i <= end_y; i++)
          {
             char *tmp = termio_selection_get(obj, start_x, i, end_x, i,
                                              &len);

             if (tmp)
               {
                  eina_strbuf_append_length(sb, tmp, len);
                  if (len && tmp[len - 1] != '\n')
                    eina_strbuf_append_char(sb, '\n');
                  free(tmp);
               }
          }
        len = eina_strbuf_length_get(sb);
        s = eina_strbuf_string_steal(sb);
        eina_strbuf_free(sb);
     }
   else if (!start_y && !end_y && !start_x && !end_x && sd->link.string)
     {
        len = strlen(sd->link.string);
        s = strndup(sd->link.string, len);
     }
   else if ((start_x != end_x) || (start_y != end_y))
     {
        if ((start_y > end_y) || ((start_y == end_y) && (end_x < start_x)))
          {
             INT_SWAP(start_y, end_y);
             INT_SWAP(start_x, end_x);
          }
        s = termio_selection_get(obj, start_x, start_y, end_x, end_y, &len);
     }

   if (s)
     {
        if ((sd->win) && (len > 0))
          _take_selection_text(obj, type, s);
        free(s);
     }
}

static Eina_Bool
_getsel_cb(void *data, Evas_Object *obj EINA_UNUSED, Elm_Selection_Data *ev)
{
   Termio *sd = evas_object_smart_data_get(data);

   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, EINA_FALSE);

   if (ev->format == ELM_SEL_FORMAT_TEXT)
     {
        if (ev->len > 0)
          {
             char *tmp, *s;
             size_t i;

             // apparently we have to convert \n into \r in terminal land.
             tmp = malloc(ev->len);
             if (tmp)
               {
                  s = ev->data;
                  for (i = 0; i < ev->len; i++)
                    {
                       tmp[i] = s[i];
                       if (tmp[i] == '\n') tmp[i] = '\r';
                    }

                  if (sd->pty->state.bracketed_paste)
                      termpty_write(sd->pty, "\x1b[200~",
                                    sizeof("\x1b[200~") - 1);

                  termpty_write(sd->pty, tmp, ev->len - 1);

                  if (sd->pty->state.bracketed_paste)
                      termpty_write(sd->pty, "\x1b[201~",
                                    sizeof("\x1b[201~") - 1);

                  free(tmp);
               }
          }
     }
   return EINA_TRUE;
}

static void
_paste_selection(Evas_Object *obj, Elm_Sel_Type type)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN(sd);
   if (!sd->win) return;
   elm_cnp_selection_get(sd->win, type, ELM_SEL_FORMAT_TEXT,
                         _getsel_cb, obj);
}

static void
_font_size_set(Evas_Object *obj, int size)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Config *config = termio_config_get(obj);
   EINA_SAFETY_ON_NULL_RETURN(sd);

   if (size < 5) size = 5;
   else if (size > 100) size = 100;
   if (config)
     {
        Evas_Coord mw = 1, mh = 1;
        int gw, gh;

        config->temporary = EINA_TRUE;
        config->font.size = size;
        gw = sd->grid.w;
        gh = sd->grid.h;
        sd->noreqsize = 1;
        termio_config_update(obj);
        sd->noreqsize = 0;
        evas_object_size_hint_min_get(obj, &mw, &mh);
        evas_object_data_del(obj, "sizedone");
        evas_object_size_hint_request_set(obj, mw * gw, mh * gh);
     }
}

void
termio_font_size_set(Evas_Object *obj, int size)
{
   _font_size_set(obj, size);
}

void
termio_grid_size_set(Evas_Object *obj, int w, int h)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Coord mw = 1, mh = 1;

   EINA_SAFETY_ON_NULL_RETURN(sd);

   if (w < 1) w = 1;
   if (h < 1) h = 1;
   evas_object_size_hint_min_get(obj, &mw, &mh);
   evas_object_data_del(obj, "sizedone");
   evas_object_size_hint_request_set(obj, mw * w, mh * h);
}

static void
_smart_cb_key_up(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event)
{
   Evas_Event_Key_Up *ev = event;
   Termio *sd = evas_object_smart_data_get(data);

   EINA_SAFETY_ON_NULL_RETURN(sd);
   sd->last_keyup = ev->timestamp;
   if (sd->imf)
     {
        Ecore_IMF_Event_Key_Up imf_ev;
        ecore_imf_evas_event_key_up_wrap(ev, &imf_ev);
        if (ecore_imf_context_filter_event
            (sd->imf, ECORE_IMF_EVENT_KEY_UP, (Ecore_IMF_Event *)&imf_ev))
          return;
     }
}

static Eina_Bool
_is_modifier(const char *key)
{
   if ((!strncmp(key, "Shift", 5)) ||
       (!strncmp(key, "Control", 7)) ||
       (!strncmp(key, "Alt", 3)) ||
       (!strncmp(key, "Meta", 4)) ||
       (!strncmp(key, "Super", 5)) ||
       (!strncmp(key, "Hyper", 5)) ||
       (!strcmp(key, "Scroll_Lock")) ||
       (!strcmp(key, "Num_Lock")) ||
       (!strcmp(key, "Caps_Lock")))
     return EINA_TRUE;
   return EINA_FALSE;
}

static void
_compose_seq_reset(Termio *sd)
{
   char *str;
   
   EINA_LIST_FREE(sd->seq, str) eina_stringshare_del(str);
   sd->composing = EINA_FALSE;
}

static Eina_Bool
_handle_alt_ctrl(const char *keyname, Evas_Object *term)
{
   if (!strcmp(keyname, "equal"))
     termcmd_do(term, NULL, NULL, "f+");
   else if (!strcmp(keyname, "minus"))
     termcmd_do(term, NULL, NULL, "f-");
   else if (!strcmp(keyname, "0"))
     termcmd_do(term, NULL, NULL, "f");
   else if (!strcmp(keyname, "9"))
     termcmd_do(term, NULL, NULL, "fb");
   else
     return EINA_FALSE;

   return EINA_TRUE;
}

static Eina_Bool
_handle_shift(Evas_Event_Key_Down *ev, int by, Evas_Object *term, Termio *sd)
{
   if (!strcmp(ev->key, "Prior"))
     {
        if (!(sd->pty->altbuf))
          {
             sd->scroll += by;
             if (sd->scroll > sd->pty->backscroll_num)
               sd->scroll = sd->pty->backscroll_num;
             _smart_update_queue(term, sd);
          }
     }
   else if (!strcmp(ev->key, "Next"))
     {
        sd->scroll -= by;
        if (sd->scroll < 0) sd->scroll = 0;
        _smart_update_queue(term, sd);
     }
   else if (!strcmp(ev->key, "Insert"))
     {
        if (evas_key_modifier_is_set(ev->modifiers, "Control"))
          _paste_selection(term, ELM_SEL_TYPE_PRIMARY);
        else
          _paste_selection(term, ELM_SEL_TYPE_CLIPBOARD);
     }
   else if (!strcmp(ev->key, "KP_Add"))
     {
        Config *config = termio_config_get(term);
        
        if (config) _font_size_set(term, config->font.size + 1);
     }
   else if (!strcmp(ev->key, "KP_Subtract"))
     {
        Config *config = termio_config_get(term);
        
        if (config) _font_size_set(term, config->font.size - 1);
     }
   else if (!strcmp(ev->key, "KP_Multiply"))
     {
        Config *config = termio_config_get(term);
        
        if (config) _font_size_set(term, 10);
     }
   else if (!strcmp(ev->key, "KP_Divide"))
     _take_selection(term, ELM_SEL_TYPE_CLIPBOARD);
   else
     return EINA_FALSE;

   return EINA_TRUE;
}

static void
_smart_cb_key_down(void *data, Evas *e EINA_UNUSED,
                   Evas_Object *obj EINA_UNUSED, void *event)
{
   Evas_Event_Key_Down *ev = event;
   Termio *sd = evas_object_smart_data_get(data);
   Ecore_Compose_State state;
   char *compres = NULL;

   EINA_SAFETY_ON_NULL_RETURN(sd);
   if ((!evas_key_modifier_is_set(ev->modifiers, "Alt")) &&
       (evas_key_modifier_is_set(ev->modifiers, "Control")) &&
       (!evas_key_modifier_is_set(ev->modifiers, "Shift")))
     {
        if (!strcmp(ev->key, "Prior"))
          {
             evas_object_smart_callback_call(data, "prev", NULL);
             goto end;
          }
        else if (!strcmp(ev->key, "Next"))
          {
             evas_object_smart_callback_call(data, "next", NULL);
             goto end;
          }
        else if (!strcmp(ev->key, "1"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,1", NULL);
             goto end;
          }
        else if (!strcmp(ev->key, "2"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,2", NULL);
             goto end;
          }
        else if (!strcmp(ev->key, "3"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,3", NULL);
             goto end;
          }
        else if (!strcmp(ev->key, "4"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,4", NULL);
             goto end;
          }
        else if (!strcmp(ev->key, "5"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,5", NULL);
             goto end;
          }
        else if (!strcmp(ev->key, "6"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,6", NULL);
             goto end;
          }
        else if (!strcmp(ev->key, "7"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,7", NULL);
             goto end;
          }
        else if (!strcmp(ev->key, "8"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,8", NULL);
             goto end;
          }
        else if (!strcmp(ev->key, "9"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,9", NULL);
             goto end;
          }
        else if (!strcmp(ev->key, "0"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "tab,0", NULL);
             goto end;
          }
     }
   if ((!evas_key_modifier_is_set(ev->modifiers, "Alt")) &&
       (evas_key_modifier_is_set(ev->modifiers, "Control")) &&
       (evas_key_modifier_is_set(ev->modifiers, "Shift")))
     {
        if (!strcmp(ev->key, "Prior"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "split,h", NULL);
             goto end;
          }
        else if (!strcmp(ev->key, "Next"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "split,v", NULL);
             goto end;
          }
        else if (!strcasecmp(ev->key, "t"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "new", NULL);
             goto end;
          }
        else if (!strcmp(ev->key, "Home"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "select", NULL);
             goto end;
          }
        else if (!strcasecmp(ev->key, "c"))
          {
             _compose_seq_reset(sd);
             _take_selection(data, ELM_SEL_TYPE_CLIPBOARD);
             goto end;
          }
        else if (!strcasecmp(ev->key, "v"))
          {
             _compose_seq_reset(sd);
             _paste_selection(data, ELM_SEL_TYPE_CLIPBOARD);
             goto end;
          }
     }
   if ((evas_key_modifier_is_set(ev->modifiers, "Alt")) &&
       (!evas_key_modifier_is_set(ev->modifiers, "Shift")) &&
       (!evas_key_modifier_is_set(ev->modifiers, "Control")))
     {
        if (!strcmp(ev->key, "Home"))
          {
             _compose_seq_reset(sd);
             evas_object_smart_callback_call(data, "cmdbox", NULL);
             goto end;
          }
        else if (!strcmp(ev->key, "Return"))
          {
             _compose_seq_reset(sd);
             _paste_selection(data, ELM_SEL_TYPE_PRIMARY);
             goto end;
          }
     }
   if ((evas_key_modifier_is_set(ev->modifiers, "Alt")) &&
       (evas_key_modifier_is_set(ev->modifiers, "Control")) &&
       (!evas_key_modifier_is_set(ev->modifiers, "Shift")))
     {
        if (_handle_alt_ctrl(ev->key, data))
          {
             _compose_seq_reset(sd);
             goto end;
          }
     }
   if (sd->imf)
     {
        // EXCEPTION. Don't filter modifiers alt+shift -> breaks emacs
        // and jed (alt+shift+5 for search/replace for example)
        // Don't filter modifiers alt, is used by shells
        if (!evas_key_modifier_is_set(ev->modifiers, "Alt"))
          {
             Ecore_IMF_Event_Key_Down imf_ev;

             ecore_imf_evas_event_key_down_wrap(ev, &imf_ev);
             if (!sd->composing)
               {
                  if (ecore_imf_context_filter_event
                      (sd->imf, ECORE_IMF_EVENT_KEY_DOWN, (Ecore_IMF_Event *)&imf_ev))
                    goto end;
               }
          }
     }
   if ((evas_key_modifier_is_set(ev->modifiers, "Shift")) &&
       (ev->key))
     {
        int by = sd->grid.h - 2;

        if (by < 1) by = 1;

        if (_handle_shift(ev, by, data, sd))
          {
             _compose_seq_reset(sd);
             goto end;
          }
     }
   if (sd->jump_on_keypress)
     {
        if (!_is_modifier(ev->key))
          {
             sd->scroll = 0;
             _smart_update_queue(data, sd);
          }
     }
   // if term app asked fro kbd lock - dont handle here
   if (sd->pty->state.kbd_lock) return;
   // if app asked us to not do autorepeat - ignore pree is it is the same
   // timestamp as last one
   if ((sd->pty->state.no_autorepeat) &&
       (ev->timestamp == sd->last_keyup)) return;
   if (!sd->composing)
     {
        _compose_seq_reset(sd);
        sd->seq = eina_list_append(sd->seq, eina_stringshare_add(ev->key));
        state = ecore_compose_get(sd->seq, &compres);
        if (state == ECORE_COMPOSE_MIDDLE) sd->composing = EINA_TRUE;
        else sd->composing = EINA_FALSE;
        if (!sd->composing) _compose_seq_reset(sd);
        else goto end;
     }
   else
     {
        if (_is_modifier(ev->key)) goto end;
        sd->seq = eina_list_append(sd->seq, eina_stringshare_add(ev->key));
        state = ecore_compose_get(sd->seq, &compres);
        if (state == ECORE_COMPOSE_NONE) _compose_seq_reset(sd);
        else if (state == ECORE_COMPOSE_DONE)
          {
             _compose_seq_reset(sd);
             if (compres)
               {
                  termpty_write(sd->pty, compres, strlen(compres));
                  free(compres);
                  compres = NULL;
               }
             goto end;
          }
        else goto end;
     }
   keyin_handle(sd->pty, ev);
end:
   if (sd->config->flicker_on_key)
     edje_object_signal_emit(sd->cursor.obj, "key,down", "terminology");
}

static void
_imf_cursor_set(Termio *sd)
{
   /* TODO */
   Evas_Coord cx, cy, cw, ch;
   evas_object_geometry_get(sd->cursor.obj, &cx, &cy, &cw, &ch);
   if (sd->imf)
     ecore_imf_context_cursor_location_set(sd->imf, cx, cy, cw, ch);
   /*
    ecore_imf_context_cursor_position_set(sd->imf, 0); // how to get it?
    */
}

static void
_smart_cb_focus_in(void *data, Evas *e EINA_UNUSED,
                   Evas_Object *obj EINA_UNUSED, void *event EINA_UNUSED)
{
   Termio *sd = evas_object_smart_data_get(data);
   EINA_SAFETY_ON_NULL_RETURN(sd);

   if (sd->config->disable_cursor_blink)
     edje_object_signal_emit(sd->cursor.obj, "focus,in,noblink", "terminology");
   else
     edje_object_signal_emit(sd->cursor.obj, "focus,in", "terminology");
   if (!sd->win) return;
   elm_win_keyboard_mode_set(sd->win, ELM_WIN_KEYBOARD_TERMINAL);
   if (sd->imf)
     {
        ecore_imf_context_input_panel_show(sd->imf);
        ecore_imf_context_reset(sd->imf);
        ecore_imf_context_focus_in(sd->imf);
        _imf_cursor_set(sd);
     }
}

static void
_smart_cb_focus_out(void *data, Evas *e EINA_UNUSED, Evas_Object *obj,
                    void *event EINA_UNUSED)
{
   Termio *sd = evas_object_smart_data_get(data);
   EINA_SAFETY_ON_NULL_RETURN(sd);

   if (sd->link.ctxpopup) return; /* ctxp triggers focus out we should ignore */

   edje_object_signal_emit(sd->cursor.obj, "focus,out", "terminology");
   if (!sd->win) return;
   elm_win_keyboard_mode_set(sd->win, ELM_WIN_KEYBOARD_OFF);
   if (sd->imf)
     {
        ecore_imf_context_reset(sd->imf);
        _imf_cursor_set(sd);
        ecore_imf_context_focus_out(sd->imf);
        ecore_imf_context_input_panel_hide(sd->imf);
     }
   _remove_links(sd, obj);
}

static void
_smart_xy_to_cursor(Evas_Object *obj, Evas_Coord x, Evas_Coord y,
                    int *cx, int *cy)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN(sd);
   Evas_Coord ox, oy;

   evas_object_geometry_get(obj, &ox, &oy, NULL, NULL);
   *cx = (x - ox) / sd->font.chw;
   *cy = (y - oy) / sd->font.chh;
   if (*cx < 0) *cx = 0;
   else if (*cx >= sd->grid.w) *cx = sd->grid.w - 1;
   if (*cy < 0) *cy = 0;
   else if (*cy >= sd->grid.h) *cy = sd->grid.h - 1;
}

static void
_sel_line(Evas_Object *obj, int cx EINA_UNUSED, int cy)
{
   int y, w = 0;
   Termio *sd = evas_object_smart_data_get(obj);
   Termcell *cells;

   EINA_SAFETY_ON_NULL_RETURN(sd);

   termpty_cellcomp_freeze(sd->pty);

   _sel_set(obj, EINA_TRUE);
   sd->pty->selection.makesel = EINA_FALSE;
   sd->pty->selection.start.x = 0;
   sd->pty->selection.start.y = cy;
   sd->pty->selection.end.x = sd->grid.w - 1;
   sd->pty->selection.end.y = cy;

   y = cy;
   for (;;)
     {
        cells = termpty_cellrow_get(sd->pty, y - 1, &w);
        if (!cells || !cells[w-1].att.autowrapped) break;

        y--;
     }
   sd->pty->selection.start.y = y;
   y = cy;

   for (;;)
     {
        cells = termpty_cellrow_get(sd->pty, y, &w);
        if (!cells || !cells[w-1].att.autowrapped) break;

        sd->pty->selection.end.x = w - 1;
        y++;
     }
   sd->pty->selection.end.y = y;

   termpty_cellcomp_thaw(sd->pty);
}

static Eina_Bool
_codepoint_is_wordsep(const Config *config, int g)
{
   int i;

   if (g == 0) return EINA_TRUE;
   if (!config->wordsep) return EINA_FALSE;
   for (i = 0;;)
     {
        int g2 = 0;

        if (!config->wordsep[i]) break;
        i = evas_string_char_next_get(config->wordsep, i, &g2);
        if (i < 0) break;
        if (g == g2) return EINA_TRUE;
     }
   return EINA_FALSE;
}

static void
_sel_word(Evas_Object *obj, int cx, int cy)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Termcell *cells;
   int x, y, w = 0;
   Eina_Bool done = EINA_FALSE;

   EINA_SAFETY_ON_NULL_RETURN(sd);

   termpty_cellcomp_freeze(sd->pty);

   _sel_set(obj, EINA_TRUE);
   sd->pty->selection.makesel = EINA_FALSE;
   sd->pty->selection.start.x = cx;
   sd->pty->selection.start.y = cy;
   sd->pty->selection.end.x = cx;
   sd->pty->selection.end.y = cy;
   x = cx;
   y = cy;
   cells = termpty_cellrow_get(sd->pty, y, &w);
   if (!cells) goto end;
   if (x >= w) x = w - 1;

   do
     {
        for (; x >= 0; x--)
          {
#if defined(SUPPORT_DBLWIDTH)
             if ((cells[x].codepoint == 0) && (cells[x].att.dblwidth) &&
                 (x > 0))
               x--;
#endif
             if (_codepoint_is_wordsep(sd->config, cells[x].codepoint))
               {
                  done = EINA_TRUE;
                  break;
               }
             sd->pty->selection.start.x = x;
             sd->pty->selection.start.y = y;
          }
        if (!done)
          {
             Termcell *old_cells = cells;

             cells = termpty_cellrow_get(sd->pty, y - 1, &w);
             if (!cells || !cells[w-1].att.autowrapped)
               {
                  x = 0;
                  cells = old_cells;
                  done = EINA_TRUE;
               }
             else
               {
                  y--;
                  x = w - 1;
               }
          }
     }
   while (!done);

   done = EINA_FALSE;
   if (cy != y)
     {
        y = cy;
        cells = termpty_cellrow_get(sd->pty, y, &w);
        if (!cells) goto end;
     }
   x = sd->pty->selection.end.x;

   do
     {
        for (; x < w; x++)
          {
#if defined(SUPPORT_DBLWIDTH)
             if ((cells[x].codepoint == 0) && (cells[x].att.dblwidth) &&
                 (x < (w - 1)))
               {
                  sd->pty->selection.end.x = x;
                  x++;
               }
#endif
             if (_codepoint_is_wordsep(sd->config, cells[x].codepoint))
               {
                  done = EINA_TRUE;
                  break;
               }
             sd->pty->selection.end.x = x;
             sd->pty->selection.end.y = y;
          }
        if (!done)
          {
             if (!cells[w - 1].att.autowrapped) goto end;
             y++;
             x = 0;
             cells = termpty_cellrow_get(sd->pty, y, &w);
             if (!cells) goto end;
          }
     }
   while (!done);

  end:
   termpty_cellcomp_thaw(sd->pty);
}

static void
_sel_word_to(Evas_Object *obj, int cx, int cy)
{
   Termio *sd = evas_object_smart_data_get(obj);
   int start_x, start_y, end_x, end_y;

   EINA_SAFETY_ON_NULL_RETURN(sd);

   start_x = sd->pty->selection.start.x;
   start_y = sd->pty->selection.start.y;
   end_x   = sd->pty->selection.end.x;
   end_y   = sd->pty->selection.end.y;

   if (sd->pty->selection.is_box)
     {
        if (start_y > end_y)
          INT_SWAP(start_y, end_y);
        if (start_x > end_x)
          INT_SWAP(start_x, end_x);
        if ((cy >= start_y && cy <= end_y) &&
            (cx >= start_x && cx <= end_x))
          {
             _sel_set(obj, EINA_FALSE);
             return;
          }
     }
   else
     {
        if ((start_y > end_y) ||
            ((start_y == end_y) && (end_x < start_x)))
          {
             INT_SWAP(start_y, end_y);
             INT_SWAP(start_x, end_x);
          }
        if ((cy > start_y || (cy == start_y && cx >= start_x)) &&
             (cy < end_y  || (cy == end_y && cx <= end_x)))
          {
             _sel_set(obj, EINA_FALSE);
             return;
          }
     }

   _sel_word(obj, cx, cy);

   if (sd->pty->selection.is_box)
     {
        start_y = MIN(start_y, sd->pty->selection.start.y);
        start_x = MIN(start_x, sd->pty->selection.start.x);
        end_y = MAX(end_y, sd->pty->selection.end.y);
        end_x = MAX(end_x, sd->pty->selection.end.x);
     }
   else
     {
        if (sd->pty->selection.start.y < start_y ||
           (sd->pty->selection.start.y == start_y &&
            sd->pty->selection.start.x < start_x))
          {
             start_x = sd->pty->selection.start.x;
             start_y = sd->pty->selection.start.y;
          }
        else
        if (sd->pty->selection.end.y > end_y ||
           (sd->pty->selection.end.y == end_y &&
            sd->pty->selection.end.x > end_x))
          {
             end_x = sd->pty->selection.end.x;
             end_y = sd->pty->selection.end.y;
          }
     }

   sd->pty->selection.start.x = start_x;
   sd->pty->selection.start.y = start_y;
   sd->pty->selection.end.x = end_x;
   sd->pty->selection.end.y = end_y;
}

static Eina_Bool
_rep_mouse_down(Termio *sd, Evas_Event_Mouse_Down *ev, int cx, int cy)
{
   char buf[64];
   Eina_Bool ret = EINA_FALSE;
   int btn;

   if (sd->pty->mouse_mode == MOUSE_OFF) return EINA_FALSE;
   if (!sd->mouse.button)
     {
        /* Need to remember the first button pressed for terminal handling */
        sd->mouse.button = ev->button;
     }

   btn = ev->button - 1;
   switch (sd->pty->mouse_ext)
     {
      case MOUSE_EXT_NONE:
        if ((cx < (0xff - ' ')) && (cy < (0xff - ' ')))
          {
             if (sd->pty->mouse_mode == MOUSE_X10)
               {
                  if (btn <= 2)
                    {
                       buf[0] = 0x1b;
                       buf[1] = '[';
                       buf[2] = 'M';
                       buf[3] = btn + ' ';
                       buf[4] = cx + 1 + ' ';
                       buf[5] = cy + 1 + ' ';
                       buf[6] = 0;
                       termpty_write(sd->pty, buf, strlen(buf));
                       ret = EINA_TRUE;
                    }
               }
             else
               {
                  int meta = evas_key_modifier_is_set(ev->modifiers, "Alt") ? 8 : 0;

                  if (btn > 2) btn = 0;
                  buf[0] = 0x1b;
                  buf[1] = '[';
                  buf[2] = 'M';
                  buf[3] = (btn | meta) + ' ';
                  buf[4] = cx + 1 + ' ';
                  buf[5] = cy + 1 + ' ';
                  buf[6] = 0;
                  termpty_write(sd->pty, buf, strlen(buf));
                  ret = EINA_TRUE;
               }
          }
        break;
      case MOUSE_EXT_UTF8: // ESC.[.M.BTN/FLGS.XUTF8.YUTF8
          {
             int meta = evas_key_modifier_is_set(ev->modifiers, "Alt") ? 8 : 0;
             int v, i;

             if (btn > 2) btn = 0;
             buf[0] = 0x1b;
             buf[1] = '[';
             buf[2] = 'M';
             buf[3] = (btn | meta) + ' ';
             i = 4;
             v = cx + 1 + ' ';
             if (v <= 127) buf[i++] = v;
             else
               { // 14 bits for cx/cy - enough i think
                   buf[i++] = 0xc0 + (v >> 6);
                   buf[i++] = 0x80 + (v & 0x3f);
               }
             v = cy + 1 + ' ';
             if (v <= 127) buf[i++] = v;
             else
               { // 14 bits for cx/cy - enough i think
                   buf[i++] = 0xc0 + (v >> 6);
                   buf[i++] = 0x80 + (v & 0x3f);
               }
             buf[i] = 0;
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      case MOUSE_EXT_SGR: // ESC.[.<.NUM.;.NUM.;.NUM.M
          {
             int meta = evas_key_modifier_is_set(ev->modifiers, "Alt") ? 8 : 0;

             snprintf(buf, sizeof(buf), "%c[<%i;%i;%iM", 0x1b,
                      (btn | meta), cx + 1, cy + 1);
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      case MOUSE_EXT_URXVT: // ESC.[.NUM.;.NUM.;.NUM.M
          {
             int meta = evas_key_modifier_is_set(ev->modifiers, "Alt") ? 8 : 0;

             if (btn > 2) btn = 0;
             snprintf(buf, sizeof(buf), "%c[%i;%i;%iM", 0x1b,
                      (btn | meta) + ' ',
                      cx + 1, cy + 1);
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      default:
        break;
     }
   return ret;
}

static Eina_Bool
_rep_mouse_up(Termio *sd, Evas_Event_Mouse_Up *ev, int cx, int cy)
{
   char buf[64];
   Eina_Bool ret = EINA_FALSE;
   int meta;

   if ((sd->pty->mouse_mode == MOUSE_OFF) ||
       (sd->pty->mouse_mode == MOUSE_X10))
     return EINA_FALSE;
   if (sd->mouse.button == ev->button)
     sd->mouse.button = 0;

   meta = evas_key_modifier_is_set(ev->modifiers, "Alt") ? 8 : 0;

   switch (sd->pty->mouse_ext)
     {
      case MOUSE_EXT_NONE:
        if ((cx < (0xff - ' ')) && (cy < (0xff - ' ')))
          {
             buf[0] = 0x1b;
             buf[1] = '[';
             buf[2] = 'M';
             buf[3] = (3 | meta) + ' ';
             buf[4] = cx + 1 + ' ';
             buf[5] = cy + 1 + ' ';
             buf[6] = 0;
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      case MOUSE_EXT_UTF8: // ESC.[.M.BTN/FLGS.XUTF8.YUTF8
          {
             int v, i;

             buf[0] = 0x1b;
             buf[1] = '[';
             buf[2] = 'M';
             buf[3] = (3 | meta) + ' ';
             i = 4;
             v = cx + 1 + ' ';
             if (v <= 127) buf[i++] = v;
             else
               { // 14 bits for cx/cy - enough i think
                   buf[i++] = 0xc0 + (v >> 6);
                   buf[i++] = 0x80 + (v & 0x3f);
               }
             v = cy + 1 + ' ';
             if (v <= 127) buf[i++] = v;
             else
               { // 14 bits for cx/cy - enough i think
                   buf[i++] = 0xc0 + (v >> 6);
                   buf[i++] = 0x80 + (v & 0x3f);
               }
             buf[i] = 0;
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      case MOUSE_EXT_SGR: // ESC.[.<.NUM.;.NUM.;.NUM.m
          {
             snprintf(buf, sizeof(buf), "%c[<%i;%i;%im", 0x1b,
                      (3 | meta), cx + 1, cy + 1);
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      case MOUSE_EXT_URXVT: // ESC.[.NUM.;.NUM.;.NUM.M
          {
             snprintf(buf, sizeof(buf), "%c[%i;%i;%iM", 0x1b,
                      (3 | meta) + ' ',
                      cx + 1, cy + 1);
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      default:
        break;
     }
   return ret;
}

static Eina_Bool
_rep_mouse_move(Termio *sd, Evas_Event_Mouse_Move *ev, int cx, int cy)
{
   char buf[64];
   Eina_Bool ret = EINA_FALSE;
   int btn, meta;

   if ((sd->pty->mouse_mode == MOUSE_OFF) ||
       (sd->pty->mouse_mode == MOUSE_X10) ||
       (sd->pty->mouse_mode == MOUSE_NORMAL))
     return EINA_FALSE;

   if ((!sd->mouse.button) && (sd->pty->mouse_mode == MOUSE_NORMAL_BTN_MOVE))
     return EINA_FALSE;

   btn = sd->mouse.button - 1;
   meta = evas_key_modifier_is_set(ev->modifiers, "Alt") ? 8 : 0;

   switch (sd->pty->mouse_ext)
     {
      case MOUSE_EXT_NONE:
        if ((cx < (0xff - ' ')) && (cy < (0xff - ' ')))
          {
             if (btn > 2) btn = 0;
             buf[0] = 0x1b;
             buf[1] = '[';
             buf[2] = 'M';
             buf[3] = (btn | meta | 32) + ' ';
             buf[4] = cx + 1 + ' ';
             buf[5] = cy + 1 + ' ';
             buf[6] = 0;
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      case MOUSE_EXT_UTF8: // ESC.[.M.BTN/FLGS.XUTF8.YUTF8
          {
             int v, i;

             if (btn > 2) btn = 0;
             buf[0] = 0x1b;
             buf[1] = '[';
             buf[2] = 'M';
             buf[3] = (btn | meta | 32) + ' ';
             i = 4;
             v = cx + 1 + ' ';
             if (v <= 127) buf[i++] = v;
             else
               { // 14 bits for cx/cy - enough i think
                   buf[i++] = 0xc0 + (v >> 6);
                   buf[i++] = 0x80 + (v & 0x3f);
               }
             v = cy + 1 + ' ';
             if (v <= 127) buf[i++] = v;
             else
               { // 14 bits for cx/cy - enough i think
                   buf[i++] = 0xc0 + (v >> 6);
                   buf[i++] = 0x80 + (v & 0x3f);
               }
             buf[i] = 0;
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      case MOUSE_EXT_SGR: // ESC.[.<.NUM.;.NUM.;.NUM.M
          {
             snprintf(buf, sizeof(buf), "%c[<%i;%i;%iM", 0x1b,
                      (btn | meta | 32), cx + 1, cy + 1);
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      case MOUSE_EXT_URXVT: // ESC.[.NUM.;.NUM.;.NUM.M
          {
             if (btn > 2) btn = 0;
             snprintf(buf, sizeof(buf), "%c[%i;%i;%iM", 0x1b,
                      (btn | meta | 32) + ' ',
                      cx + 1, cy + 1);
             termpty_write(sd->pty, buf, strlen(buf));
             ret = EINA_TRUE;
          }
        break;
      default:
        break;
     }
   return ret;
}

static void
_selection_dbl_fix(Evas_Object *obj
#if defined(SUPPORT_DBLWIDTH)
                   EINA_UNUSED
#endif
                   )
{
#if defined(SUPPORT_DBLWIDTH)
   Termio *sd = evas_object_smart_data_get(obj);
   int w = 0;
   Termcell *cells;

   EINA_SAFETY_ON_NULL_RETURN(sd);
   termpty_cellcomp_freeze(sd->pty);
   cells = termpty_cellrow_get(sd->pty, sd->pty->selection.end.y - sd->scroll, &w);
   if (cells)
     {
        // if sel2 after sel1
        if ((sd->pty->selection.end.y > sd->pty->selection.start.y) ||
            ((sd->pty->selection.end.y == sd->pty->selection.start.y) &&
                (sd->pty->selection.end.x >= sd->pty->selection.start.x)))
          {
             if (sd->pty->selection.end.x < (w - 1))
               {
                  if ((cells[sd->pty->selection.end.x].codepoint != 0) &&
                      (cells[sd->pty->selection.end.x].att.dblwidth))
                    sd->pty->selection.end.x++;
               }
          }
        // else sel1 after sel 2
        else
          {
             if (sd->pty->selection.end.x > 0)
               {
                  if ((cells[sd->pty->selection.end.x].codepoint == 0) &&
                      (cells[sd->pty->selection.end.x].att.dblwidth))
                    sd->pty->selection.end.x--;
               }
          }
     }
   cells = termpty_cellrow_get(sd->pty, sd->pty->selection.start.y - sd->scroll, &w);
   if (cells)
     {
        // if sel2 after sel1
        if ((sd->pty->selection.end.y > sd->pty->selection.start.y) ||
            ((sd->pty->selection.end.y == sd->pty->selection.start.y) &&
                (sd->pty->selection.end.x >= sd->pty->selection.start.x)))
          {
             if (sd->pty->selection.start.x > 0)
               {
                  if ((cells[sd->pty->selection.start.x].codepoint == 0) &&
                      (cells[sd->pty->selection.start.x].att.dblwidth))
                    sd->pty->selection.start.x--;
               }
          }
        // else sel1 after sel 2
        else
          {
             if (sd->pty->selection.start.x < (w - 1))
               {
                  if ((cells[sd->pty->selection.start.x].codepoint != 0) &&
                      (cells[sd->pty->selection.start.x].att.dblwidth))
                    sd->pty->selection.start.x++;
               }
          }
     }
   termpty_cellcomp_thaw(sd->pty);
#endif
}

static void
_selection_newline_extend_fix(Evas_Object *obj)
{
   Termio *sd;
   
   sd = evas_object_smart_data_get(obj);
   if ((!sd->top_left) && (sd->pty->selection.end.y >= sd->pty->selection.start.y))
     {
        if (((sd->pty->selection.start.y == sd->pty->selection.end.y) &&
             (sd->pty->selection.start.x <= sd->pty->selection.end.x)) ||
            (sd->pty->selection.start.y < sd->pty->selection.end.y))
          {
             char *lastline;
             int x1, y1, x2, y2;
             size_t len;

             if (sd->pty->selection.start.y == sd->pty->selection.end.y) x1 = sd->pty->selection.start.x;
             else x1 = 0;
             x2 = sd->pty->selection.end.x;
             y1 = y2 = sd->pty->selection.end.y;
             lastline = termio_selection_get(obj, x1, y1, x2, y2, &len);
             if (lastline)
               {
                  if ((len > 0) && (lastline[len - 1] == '\n'))
                    {
                       sd->pty->selection.end.x = sd->grid.w - 1;
                       _selection_dbl_fix(obj);
                    }
                  free(lastline);
               }
          }
     }
}

static void
_smart_cb_mouse_move_job(void *data)
{
   Termio *sd = evas_object_smart_data_get(data);

   EINA_SAFETY_ON_NULL_RETURN(sd);
   sd->mouse_move_job = NULL;
   if (sd->mouseover_delay) ecore_timer_del(sd->mouseover_delay);
   sd->mouseover_delay = ecore_timer_add(0.05, _smart_mouseover_delay, data);
}

static void
_edje_cb_bottom_right_in(void *data, Evas_Object *obj EINA_UNUSED,
                         const char *emission EINA_UNUSED, const char *source EINA_UNUSED)
{
   Termio *sd = data;

   sd->bottom_right = EINA_TRUE;
}

static void
_edje_cb_top_left_in(void *data, Evas_Object *obj EINA_UNUSED,
                     const char *emission EINA_UNUSED, const char *source EINA_UNUSED)
{
   Termio *sd = data;

   sd->top_left = EINA_TRUE;
}

static void
_edje_cb_bottom_right_out(void *data, Evas_Object *obj EINA_UNUSED,
                          const char *emission EINA_UNUSED, const char *source EINA_UNUSED)
{
   Termio *sd = data;

   sd->bottom_right = EINA_FALSE;
}

static void
_edje_cb_top_left_out(void *data, Evas_Object *obj EINA_UNUSED,
                      const char *emission EINA_UNUSED, const char *source EINA_UNUSED)
{
   Termio *sd = data;

   sd->top_left = EINA_FALSE;
}

static void
_smart_cb_mouse_down(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event)
{
   Evas_Event_Mouse_Down *ev = event;
   Termio *sd = evas_object_smart_data_get(data);
   int cx, cy;
   int shift, ctrl;

   EINA_SAFETY_ON_NULL_RETURN(sd);

   shift = evas_key_modifier_is_set(ev->modifiers, "Shift");
   ctrl = evas_key_modifier_is_set(ev->modifiers, "Control");
   _smart_xy_to_cursor(data, ev->canvas.x, ev->canvas.y, &cx, &cy);
   sd->didclick = EINA_FALSE;
   if ((ev->button == 3) && ctrl)
     {
        evas_object_smart_callback_call(data, "options", NULL);
        return;
     }
   if ((ev->button == 3) && shift)
     {
        termio_debugwhite_set(data, !sd->debugwhite);
        printf("debugwhite %i\n",  sd->debugwhite);
        return;
     }
   if (!shift && !ctrl)
     if (_rep_mouse_down(sd, ev, cx, cy)) return;
   if (ev->button == 1)
     {
        if (ev->flags & EVAS_BUTTON_TRIPLE_CLICK)
          {
             _sel_line(data, cx, cy - sd->scroll);
             if (sd->pty->selection.is_active)
               _take_selection(data, ELM_SEL_TYPE_PRIMARY);
             sd->didclick = EINA_TRUE;
          }
        else if (ev->flags & EVAS_BUTTON_DOUBLE_CLICK)
          {
             if (shift && sd->pty->selection.is_active)
                  _sel_word_to(data, cx, cy - sd->scroll);
             else
                  _sel_word(data, cx, cy - sd->scroll);
             if (sd->pty->selection.is_active)
               _take_selection(data, ELM_SEL_TYPE_PRIMARY);
             sd->didclick = EINA_TRUE;
          }
        else
          {
             /* SINGLE CLICK */
             if (sd->pty->selection.is_active &&
                 (sd->top_left || sd->bottom_right))
               {
                  /* stretch selection */
                  int start_x, start_y, end_x, end_y;

                  start_x = sd->pty->selection.start.x;
                  start_y = sd->pty->selection.start.y;
                  end_x   = sd->pty->selection.end.x;
                  end_y   = sd->pty->selection.end.y;
                  sd->pty->selection.makesel = EINA_TRUE;
                  if (sd->pty->selection.is_box)
                    {
                       if (sd->top_left)
                         {
                            if (start_y < end_y)
                              INT_SWAP(start_y, end_y);
                            if (start_x < end_x)
                              INT_SWAP(start_x, end_x);
                         }
                       else
                         {
                            if (start_y > end_y)
                              INT_SWAP(start_y, end_y);
                            if (start_x > end_x)
                              INT_SWAP(start_x, end_x);
                         }
                    }
                  else
                    {
                       if (sd->top_left)
                         {
                            if ((start_y < end_y) ||
                                ((start_y == end_y) && (end_x > start_x)))
                              {
                                 INT_SWAP(start_y, end_y);
                                 INT_SWAP(start_x, end_x);
                              }
                         }
                       else
                         {
                            if ((start_y > end_y) ||
                                ((start_y == end_y) && (end_x < start_x)))
                              {
                                 INT_SWAP(start_y, end_y);
                                 INT_SWAP(start_x, end_x);
                              }
                         }
                    }
                  sd->pty->selection.start.x = start_x;
                  sd->pty->selection.start.y = start_y;
                  sd->pty->selection.end.x = cx;
                  sd->pty->selection.end.y = cy - sd->scroll;
                  _selection_dbl_fix(data);
               }
             else
               {
                  sd->moved = EINA_FALSE;
                  _sel_set(data, EINA_FALSE);
                  if (!shift)
                    {
                       sd->pty->selection.is_box =
                          (ctrl ||
                           evas_key_modifier_is_set(ev->modifiers, "Alt"));
                       sd->pty->selection.start.x = cx;
                       sd->pty->selection.start.y = cy - sd->scroll;
                       sd->pty->selection.end.x = cx;
                       sd->pty->selection.end.y = cy - sd->scroll;
                       sd->pty->selection.makesel = EINA_TRUE;
                       _selection_dbl_fix(data);
                    }
               }
          }
        _smart_update_queue(data, sd);
     }
   else if (ev->button == 2)
     {
        _paste_selection(data, ELM_SEL_TYPE_PRIMARY);
     }
   else if (ev->button == 3)
     {
        elm_object_focus_set(data, EINA_TRUE);
        if (!sd->link.string)
          evas_object_smart_callback_call(data, "options", NULL);
     }
}

static void
_smart_cb_mouse_up(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event)
{
   Evas_Event_Mouse_Up *ev = event;
   Termio *sd = evas_object_smart_data_get(data);
   int cx, cy;
   int shift, ctrl;

   EINA_SAFETY_ON_NULL_RETURN(sd);

   shift = evas_key_modifier_is_set(ev->modifiers, "Shift");
   ctrl = evas_key_modifier_is_set(ev->modifiers, "Control");

   _smart_xy_to_cursor(data, ev->canvas.x, ev->canvas.y, &cx, &cy);
   if (!shift && !ctrl)
      if (_rep_mouse_up(sd, ev, cx, cy)) return;
   if (sd->link.down.dnd) return;
   if (sd->pty->selection.makesel)
     {
        if (sd->mouse_selection_scroll)
          {
             ecore_timer_del(sd->mouse_selection_scroll);
             sd->mouse_selection_scroll = NULL;
          }
        sd->pty->selection.makesel = EINA_FALSE;

        if (((sd->pty->selection.start.x == sd->pty->selection.end.x) &&
            (sd->pty->selection.start.y == sd->pty->selection.end.y)) ||
            (!sd->moved))
          {
             _sel_set(data, EINA_FALSE);
             sd->didclick = EINA_FALSE;
             _smart_update_queue(data, sd);
             return;
          }

        if (sd->pty->selection.is_active)
          {
             sd->didclick = EINA_TRUE;
             sd->pty->selection.end.x = cx;
             sd->pty->selection.end.y = cy - sd->scroll;
             _selection_dbl_fix(data);
             if (sd->pty->selection.is_box)
              {
                 sd->pty->selection.end.x = cx;
                 sd->pty->selection.end.y = cy - sd->scroll;
                 _smart_update_queue(data, sd);
                 _take_selection(data, ELM_SEL_TYPE_PRIMARY);
              }
            else
              {
                 _selection_newline_extend_fix(data);
                 _smart_update_queue(data, sd);
                 _take_selection(data, ELM_SEL_TYPE_PRIMARY);
              }
          }
     }
}

static Eina_Bool
_mouse_selection_scroll(void *data)
{
   Evas_Object *obj = data;
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Coord oy, my;
   int cy;

   if (!sd->pty->selection.makesel) return EINA_FALSE;

   evas_pointer_canvas_xy_get(evas_object_evas_get(obj), NULL, &my);
   evas_object_geometry_get(data, NULL, &oy, NULL, NULL);
   cy = (my - oy) / sd->font.chh;
   if (cy < 0)
     {
        sd->scroll -= cy;
        if (sd->scroll > sd->pty->backscroll_num)
          sd->scroll = sd->pty->backscroll_num;
        sd->pty->selection.end.y = -sd->scroll;
        _smart_update_queue(data, sd);
     }
   else if (cy >= sd->grid.h)
     {
        sd->scroll -= cy - sd->grid.h;
        if (sd->scroll < 0) sd->scroll = 0;
        sd->pty->selection.end.y = sd->scroll + sd->grid.h - 1;
        _smart_update_queue(data, sd);
     }

   return EINA_TRUE;
}

static void
_smart_cb_mouse_move(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event)
{
   Evas_Event_Mouse_Move *ev = event;
   Termio *sd = evas_object_smart_data_get(data);
   int cx, cy;
   Evas_Coord ox, oy;
   Eina_Bool scroll = EINA_FALSE;
   int shift, ctrl;

   shift = evas_key_modifier_is_set(ev->modifiers, "Shift");
   ctrl = evas_key_modifier_is_set(ev->modifiers, "Control");

   EINA_SAFETY_ON_NULL_RETURN(sd);

   evas_object_geometry_get(data, &ox, &oy, NULL, NULL);
   cx = (ev->cur.canvas.x - ox) / sd->font.chw;
   cy = (ev->cur.canvas.y - oy) / sd->font.chh;
   if (cx < 0) cx = 0;
   else if (cx >= sd->grid.w) cx = sd->grid.w - 1;
   if (cy < 0)
     {
        cy = 0;
        if (sd->pty->selection.makesel)
             scroll = EINA_TRUE;
     }
   else if (cy >= sd->grid.h)
     {
        cy = sd->grid.h - 1;
        if (sd->pty->selection.makesel)
             scroll = EINA_TRUE;
     }
   if (scroll == EINA_TRUE)
     {
        if (!sd->mouse_selection_scroll) {
             sd->mouse_selection_scroll
                = ecore_timer_add(0.05, _mouse_selection_scroll, data);
        }
        return;
     }
   else if (sd->mouse_selection_scroll)
     {
        ecore_timer_del(sd->mouse_selection_scroll);
        sd->mouse_selection_scroll = NULL;
     }

   if ((sd->mouse.cx == cx) && (sd->mouse.cy == cy)) return;

   sd->mouse.cx = cx;
   sd->mouse.cy = cy;
   if (!shift && !ctrl)
     if (_rep_mouse_move(sd, ev, cx, cy)) return;
   if (sd->link.down.dnd)
     {
        sd->pty->selection.makesel = EINA_FALSE;
        _sel_set(data, EINA_FALSE);
        _smart_update_queue(data, sd);
        return;
     }
   if (sd->pty->selection.makesel)
     {
        int start_x, start_y, end_x, end_y;

        if (!sd->pty->selection.is_active)
          {
             if ((cx != sd->pty->selection.start.x) ||
                 ((cy - sd->scroll) != sd->pty->selection.start.y))
               _sel_set(data, EINA_TRUE);
          }
        start_x = sd->pty->selection.start.x;
        start_y = sd->pty->selection.start.y;
        end_x   = sd->pty->selection.end.x;
        end_y   = sd->pty->selection.end.y;
        if ((start_y > end_y) || ((start_y == end_y) && (end_x < start_x)))
          {
             INT_SWAP(start_y, end_y);
             INT_SWAP(start_x, end_x);
          }
        cy -= sd->scroll;
        sd->top_left = EINA_FALSE;
        sd->bottom_right = EINA_FALSE;
        sd->pty->selection.end.x = cx;
        sd->pty->selection.end.y = cy;

        _selection_dbl_fix(data);
        if (!sd->pty->selection.is_box)
          _selection_newline_extend_fix(data);
        _smart_update_queue(data, sd);
        sd->moved = EINA_TRUE;
     }
   /* TODO: make the following useless */
   if (sd->mouse_move_job) ecore_job_del(sd->mouse_move_job);
   sd->mouse_move_job = ecore_job_add(_smart_cb_mouse_move_job, data);
}

static void
_smart_cb_mouse_in(void *data, Evas *e EINA_UNUSED,
                   Evas_Object *obj EINA_UNUSED, void *event)
{
   int cx, cy;
   Evas_Event_Mouse_In *ev = event;
   Termio *sd = evas_object_smart_data_get(data);

   EINA_SAFETY_ON_NULL_RETURN(sd);
   _smart_xy_to_cursor(data, ev->canvas.x, ev->canvas.y, &cx, &cy);
   sd->mouse.cx = cx;
   sd->mouse.cy = cy;
   termio_mouseover_suspend_pushpop(data, -1);
}

static void
_smart_cb_mouse_out(void *data, Evas *e EINA_UNUSED, Evas_Object *obj,
                    void *event)
{
   Termio *sd = evas_object_smart_data_get(data);
   Evas_Event_Mouse_Out *ev = event;

   EINA_SAFETY_ON_NULL_RETURN(sd);

   if (sd->link.ctxpopup) return; /* ctxp triggers mouse out we should ignore */

   termio_mouseover_suspend_pushpop(data, 1);
   ty_dbus_link_hide();
   if ((ev->canvas.x == 0) || (ev->canvas.y == 0))
     {
        sd->mouse.cx = -1;
        sd->mouse.cy = -1;
        sd->link.suspend = EINA_FALSE;
     }
   else
     {
        int cx, cy;

        _smart_xy_to_cursor(data, ev->canvas.x, ev->canvas.y, &cx, &cy);
        sd->mouse.cx = cx;
        sd->mouse.cy = cy;
     }
   _remove_links(sd, obj);

   if (sd->mouseover_delay) ecore_timer_del(sd->mouseover_delay);
   sd->mouseover_delay = NULL;
}

static void
_smart_cb_mouse_wheel(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event)
{
   Evas_Event_Mouse_Wheel *ev = event;
   Termio *sd = evas_object_smart_data_get(data);
   char buf[64];

   EINA_SAFETY_ON_NULL_RETURN(sd);

   /* do not handle horizontal scrolling */
   if (ev->direction) return;

   if (evas_key_modifier_is_set(ev->modifiers, "Control")) return;
   if (evas_key_modifier_is_set(ev->modifiers, "Alt")) return;
   if (evas_key_modifier_is_set(ev->modifiers, "Shift")) return;

   if (sd->pty->mouse_mode == MOUSE_OFF)
     {
        if (sd->pty->altbuf)
          {
             /* Emulate cursors */
             buf[0] = 0x1b;
             buf[1] = 'O';
             buf[2] = (ev->z < 0) ? 'A' : 'B';
             buf[3] = 0;
             termpty_write(sd->pty, buf, strlen(buf));
          }
        else
          {
             sd->scroll -= (ev->z * 4);
             if (sd->scroll > sd->pty->backscroll_num)
               sd->scroll = sd->pty->backscroll_num;
             else if (sd->scroll < 0) sd->scroll = 0;
             _smart_update_queue(data, sd);
          }
     }
   else
     {
       int cx, cy;

       _smart_xy_to_cursor(data, ev->canvas.x, ev->canvas.y, &cx, &cy);

       switch (sd->pty->mouse_ext)
         {
          case MOUSE_EXT_NONE:
            if ((cx < (0xff - ' ')) && (cy < (0xff - ' ')))
              {
                 int btn = (ev->z >= 0) ? 1 + 64 : 64;

                 buf[0] = 0x1b;
                 buf[1] = '[';
                 buf[2] = 'M';
                 buf[3] = btn + ' ';
                 buf[4] = cx + 1 + ' ';
                 buf[5] = cy + 1 + ' ';
                 buf[6] = 0;
                 termpty_write(sd->pty, buf, strlen(buf));
              }
            break;
          case MOUSE_EXT_UTF8: // ESC.[.M.BTN/FLGS.XUTF8.YUTF8
              {
                 int v, i;
                 int btn = (ev->z >= 0) ? 'a' : '`';

                 buf[0] = 0x1b;
                 buf[1] = '[';
                 buf[2] = 'M';
                 buf[3] = btn;
                 i = 4;
                 v = cx + 1 + ' ';
                 if (v <= 127) buf[i++] = v;
                 else
                   { // 14 bits for cx/cy - enough i think
                       buf[i++] = 0xc0 + (v >> 6);
                       buf[i++] = 0x80 + (v & 0x3f);
                   }
                 v = cy + 1 + ' ';
                 if (v <= 127) buf[i++] = v;
                 else
                   { // 14 bits for cx/cy - enough i think
                       buf[i++] = 0xc0 + (v >> 6);
                       buf[i++] = 0x80 + (v & 0x3f);
                   }
                 buf[i] = 0;
                 termpty_write(sd->pty, buf, strlen(buf));
              }
            break;
          case MOUSE_EXT_SGR: // ESC.[.<.NUM.;.NUM.;.NUM.M
              {
                 int btn = (ev->z >= 0) ? 1 + 64 : 64;
                 snprintf(buf, sizeof(buf), "%c[<%i;%i;%iM", 0x1b,
                          btn, cx + 1, cy + 1);
                 termpty_write(sd->pty, buf, strlen(buf));
              }
            break;
          case MOUSE_EXT_URXVT: // ESC.[.NUM.;.NUM.;.NUM.M
              {
                 int btn = (ev->z >= 0) ? 1 + 64 : 64;
                 snprintf(buf, sizeof(buf), "%c[%i;%i;%iM", 0x1b,
                          btn + ' ',
                          cx + 1, cy + 1);
                 termpty_write(sd->pty, buf, strlen(buf));
              }
            break;
          default:
            break;
         }
     }
}

static void
_win_obj_del(void *data, Evas *e EINA_UNUSED, Evas_Object *obj, void *event EINA_UNUSED)
{
   Termio *sd = evas_object_smart_data_get(data);

   EINA_SAFETY_ON_NULL_RETURN(sd);
   if (obj == sd->win)
     {
        evas_object_event_callback_del_full(sd->win, EVAS_CALLBACK_DEL,
                                            _win_obj_del, data);
        sd->win = NULL;
     }
}

void
termio_config_set(Evas_Object *obj, Config *config)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Coord w = 2, h = 2;

   sd->config = config;

   sd->jump_on_change = config->jump_on_change;
   sd->jump_on_keypress = config->jump_on_keypress;

   if (config->font.bitmap)
     {
        char buf[PATH_MAX];
        snprintf(buf, sizeof(buf), "%s/fonts/%s",
                 elm_app_data_dir_get(), config->font.name);
        sd->font.name = eina_stringshare_add(buf);
     }
   else
     sd->font.name = eina_stringshare_add(config->font.name);
   sd->font.size = config->font.size;

   evas_object_scale_set(sd->grid.obj, elm_config_scale_get());
   evas_object_textgrid_font_set(sd->grid.obj, sd->font.name, sd->font.size);
   evas_object_textgrid_size_get(sd->grid.obj, &w, &h);
   if (w < 1) w = 1;
   if (h < 1) h = 1;
   evas_object_textgrid_size_set(sd->grid.obj, w, h);
   evas_object_textgrid_cell_size_get(sd->grid.obj, &w, &h);
   if (w < 1) w = 1;
   if (h < 1) h = 1;
   sd->font.chw = w;
   sd->font.chh = h;

   theme_apply(sd->cursor.obj, config, "terminology/cursor");
   theme_auto_reload_enable(sd->cursor.obj);
   evas_object_resize(sd->cursor.obj, sd->font.chw, sd->font.chh);
   evas_object_show(sd->cursor.obj);

   theme_apply(sd->sel.theme, config, "terminology/selection");
   theme_auto_reload_enable(sd->sel.theme);
   edje_object_part_swallow(sd->sel.theme, "terminology.top_left", sd->sel.top);
   edje_object_part_swallow(sd->sel.theme, "terminology.bottom_right", sd->sel.bottom);
}

static void
_cursor_cb_move(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event EINA_UNUSED)
{
   Termio *sd = evas_object_smart_data_get(data);
   EINA_SAFETY_ON_NULL_RETURN(sd);
   _imf_cursor_set(sd);
}

static Evas_Event_Flags
_smart_cb_gest_long_move(void *data, void *event EINA_UNUSED)
{
//   Elm_Gesture_Taps_Info *p = event;
   Termio *sd = evas_object_smart_data_get(data);

   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, EVAS_EVENT_FLAG_ON_HOLD);
   evas_object_smart_callback_call(data, "options", NULL);
   sd->didclick = EINA_TRUE;
   return EVAS_EVENT_FLAG_ON_HOLD;
}

static Evas_Event_Flags
_smart_cb_gest_zoom_start(void *data, void *event)
{
   Elm_Gesture_Zoom_Info *p = event;
   Termio *sd = evas_object_smart_data_get(data);
   Config *config = termio_config_get(data);

   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, EVAS_EVENT_FLAG_ON_HOLD);
   if (config)
     {
        int sz;
        
        sd->zoom_fontsize_start = config->font.size;
        sz = (double)sd->zoom_fontsize_start * p->zoom;
        if (sz != config->font.size) _font_size_set(data, sz);
     }
   sd->didclick = EINA_TRUE;
   return EVAS_EVENT_FLAG_ON_HOLD;
}

static Evas_Event_Flags
_smart_cb_gest_zoom_move(void *data, void *event)
{
   Elm_Gesture_Zoom_Info *p = event;
   Termio *sd = evas_object_smart_data_get(data);
   Config *config = termio_config_get(data);

   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, EVAS_EVENT_FLAG_ON_HOLD);
   if (config)
     {
        int sz = (double)sd->zoom_fontsize_start *
          (1.0 + ((p->zoom - 1.0) / 30.0));
        if (sz != config->font.size) _font_size_set(data, sz);
     }
   sd->didclick = EINA_TRUE;
   return EVAS_EVENT_FLAG_ON_HOLD;
}

static Evas_Event_Flags
_smart_cb_gest_zoom_end(void *data, void *event)
{
   Elm_Gesture_Zoom_Info *p = event;
   Termio *sd = evas_object_smart_data_get(data);
   Config *config = termio_config_get(data);

   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, EVAS_EVENT_FLAG_ON_HOLD);
   if (config)
     {
        int sz = (double)sd->zoom_fontsize_start *
          (1.0 + ((p->zoom - 1.0) / 30.0));
        if (sz != config->font.size) _font_size_set(data, sz);
     }
   sd->didclick = EINA_TRUE;
   return EVAS_EVENT_FLAG_ON_HOLD;
}

static Evas_Event_Flags
_smart_cb_gest_zoom_abort(void *data, void *event EINA_UNUSED)
{
//   Elm_Gesture_Zoom_Info *p = event;
   Termio *sd = evas_object_smart_data_get(data);
   Config *config = termio_config_get(data);

   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, EVAS_EVENT_FLAG_ON_HOLD);
   if (config)
     {
        if (sd->zoom_fontsize_start != config->font.size)
          _font_size_set(data, sd->zoom_fontsize_start);
     }
   sd->didclick = EINA_TRUE;
   return EVAS_EVENT_FLAG_ON_HOLD;
}

static void
_imf_event_commit_cb(void *data, Ecore_IMF_Context *ctx EINA_UNUSED, void *event)
{
   Termio *sd = data;
   char *str = event;
   DBG("IMF committed '%s'", str);
   if (!str) return;
   termpty_write(sd->pty, str, strlen(str));
}

static void
_smart_add(Evas_Object *obj)
{
   Termio *sd;
   Evas_Object *o;

   sd = calloc(1, sizeof(Termio));
   EINA_SAFETY_ON_NULL_RETURN(sd);
   evas_object_smart_data_set(obj, sd);

   _parent_sc.add(obj);
   sd->self = obj;

   /* Terminal output widget */
   o = evas_object_textgrid_add(evas_object_evas_get(obj));
   evas_object_pass_events_set(o, EINA_TRUE);
   evas_object_propagate_events_set(o, EINA_FALSE);
   evas_object_smart_member_add(o, obj);
   evas_object_show(o);
   sd->grid.obj = o;

   /* Setup cursor */
   o = edje_object_add(evas_object_evas_get(obj));
   evas_object_pass_events_set(o, EINA_TRUE);
   evas_object_propagate_events_set(o, EINA_FALSE);
   evas_object_smart_member_add(o, obj);
   sd->cursor.obj = o;

   evas_object_event_callback_add(o, EVAS_CALLBACK_MOVE, _cursor_cb_move, obj);

   /* Setup the selection widget */
   o = evas_object_rectangle_add(evas_object_evas_get(obj));
   evas_object_pass_events_set(o, EINA_TRUE);
   evas_object_propagate_events_set(o, EINA_FALSE);
   sd->sel.top = o;
   o = evas_object_rectangle_add(evas_object_evas_get(obj));
   evas_object_pass_events_set(o, EINA_TRUE);
   evas_object_propagate_events_set(o, EINA_FALSE);
   sd->sel.bottom = o;
   o = edje_object_add(evas_object_evas_get(obj));
   evas_object_smart_member_add(o, obj);
   sd->sel.theme = o;
   edje_object_signal_callback_add(o, "mouse,in", "zone.bottom_right", _edje_cb_bottom_right_in, sd);
   edje_object_signal_callback_add(o, "mouse,in", "zone.top_left", _edje_cb_top_left_in, sd);
   edje_object_signal_callback_add(o, "mouse,out", "zone.bottom_right", _edje_cb_bottom_right_out, sd);
   edje_object_signal_callback_add(o, "mouse,out", "zone.top_left", _edje_cb_top_left_out, sd);

   /* Setup event catcher */
   o = evas_object_rectangle_add(evas_object_evas_get(obj));
   evas_object_repeat_events_set(o, EINA_TRUE);
   evas_object_smart_member_add(o, obj);
   sd->event = o;
   evas_object_color_set(o, 0, 0, 0, 0);
   evas_object_show(o);

   evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_DOWN,
                                  _smart_cb_mouse_down, obj);
   evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_UP,
                                  _smart_cb_mouse_up, obj);
   evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_MOVE,
                                  _smart_cb_mouse_move, obj);
   evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_IN,
                                  _smart_cb_mouse_in, obj);
   evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_OUT,
                                  _smart_cb_mouse_out, obj);
   evas_object_event_callback_add(o, EVAS_CALLBACK_MOUSE_WHEEL,
                                  _smart_cb_mouse_wheel, obj);

   evas_object_event_callback_add(obj, EVAS_CALLBACK_KEY_DOWN,
                                  _smart_cb_key_down, obj);
   evas_object_event_callback_add(obj, EVAS_CALLBACK_KEY_UP,
                                  _smart_cb_key_up, obj);
   evas_object_event_callback_add(obj, EVAS_CALLBACK_FOCUS_IN,
                                  _smart_cb_focus_in, obj);
   evas_object_event_callback_add(obj, EVAS_CALLBACK_FOCUS_OUT,
                                  _smart_cb_focus_out, obj);

   sd->link.suspend = 1;
   
   if (ecore_imf_init())
     {
        const char *imf_id = ecore_imf_context_default_id_get();
        Evas *e;

        if (!imf_id) sd->imf = NULL;
        else
          {
             const Ecore_IMF_Context_Info *imf_info;

             imf_info = ecore_imf_context_info_by_id_get(imf_id);
             if ((!imf_info->canvas_type) ||
                 (strcmp(imf_info->canvas_type, "evas") == 0))
               sd->imf = ecore_imf_context_add(imf_id);
             else
               {
                  imf_id = ecore_imf_context_default_id_by_canvas_type_get("evas");
                  if (imf_id) sd->imf = ecore_imf_context_add(imf_id);
               }
          }

        if (!sd->imf) goto imf_done;

        e = evas_object_evas_get(o);
        ecore_imf_context_client_window_set
          (sd->imf, (void *)ecore_evas_window_get(ecore_evas_ecore_evas_get(e)));
        ecore_imf_context_client_canvas_set(sd->imf, e);

        ecore_imf_context_event_callback_add
          (sd->imf, ECORE_IMF_CALLBACK_COMMIT, _imf_event_commit_cb, sd);

        /* make IMF usable by a terminal - no preedit, prediction... */
        ecore_imf_context_use_preedit_set
          (sd->imf, EINA_FALSE);
        ecore_imf_context_prediction_allow_set
          (sd->imf, EINA_FALSE);
        ecore_imf_context_autocapital_type_set
          (sd->imf, ECORE_IMF_AUTOCAPITAL_TYPE_NONE);
        ecore_imf_context_input_panel_layout_set
          (sd->imf, ECORE_IMF_INPUT_PANEL_LAYOUT_TERMINAL);
        ecore_imf_context_input_mode_set
          (sd->imf, ECORE_IMF_INPUT_MODE_FULL);
        ecore_imf_context_input_panel_language_set
          (sd->imf, ECORE_IMF_INPUT_PANEL_LANG_ALPHABET);
        ecore_imf_context_input_panel_return_key_type_set
          (sd->imf, ECORE_IMF_INPUT_PANEL_RETURN_KEY_TYPE_DEFAULT);
imf_done:
        if (sd->imf) DBG("Ecore IMF Setup");
        else WRN("Ecore IMF failed");
     }
   terms = eina_list_append(terms, obj);
}

static void
_smart_del(Evas_Object *obj)
{
   Evas_Object *o;
   Termio *sd = evas_object_smart_data_get(obj);
   char *chid;
   
   EINA_SAFETY_ON_NULL_RETURN(sd);
   terms = eina_list_remove(terms, obj);
   EINA_LIST_FREE(sd->mirrors, o)
     {
        evas_object_event_callback_del_full(o, EVAS_CALLBACK_DEL,
                                            _smart_mirror_del, obj);
        evas_object_del(o);
     }
   if (sd->imf)
     {
        ecore_imf_context_event_callback_del
          (sd->imf, ECORE_IMF_CALLBACK_COMMIT, _imf_event_commit_cb);
        ecore_imf_context_del(sd->imf);
     }
   if (sd->cursor.obj) evas_object_del(sd->cursor.obj);
   if (sd->event) evas_object_del(sd->event);
   if (sd->sel.top) evas_object_del(sd->sel.top);
   if (sd->sel.bottom) evas_object_del(sd->sel.bottom);
   if (sd->sel.theme) evas_object_del(sd->sel.theme);
   if (sd->anim) ecore_animator_del(sd->anim);
   if (sd->delayed_size_timer) ecore_timer_del(sd->delayed_size_timer);
   if (sd->link_do_timer) ecore_timer_del(sd->link_do_timer);
   if (sd->mouse_move_job) ecore_job_del(sd->mouse_move_job);
   if (sd->mouseover_delay) ecore_timer_del(sd->mouseover_delay);
   if (sd->font.name) eina_stringshare_del(sd->font.name);
   if (sd->pty) termpty_free(sd->pty);
   if (sd->link.string) free(sd->link.string);
   if (sd->glayer) evas_object_del(sd->glayer);
   if (sd->win)
     evas_object_event_callback_del_full(sd->win, EVAS_CALLBACK_DEL,
                                         _win_obj_del, obj);
   EINA_LIST_FREE(sd->link.objs, o)
     {
        if (o == sd->link.down.dndobj) sd->link.down.dndobj = NULL;
        evas_object_del(o);
     }
   if (sd->link.down.dndobj) evas_object_del(sd->link.down.dndobj);
   _compose_seq_reset(sd);
   if (sd->sel_str) eina_stringshare_del(sd->sel_str);
   if (sd->sel_reset_job) ecore_job_del(sd->sel_reset_job);
   EINA_LIST_FREE(sd->cur_chids, chid) eina_stringshare_del(chid);
   sd->sel_str = NULL;
   sd->sel_reset_job = NULL;
   sd->link.down.dndobj = NULL;
   sd->cursor.obj = NULL;
   sd->event = NULL;
   sd->sel.top = NULL;
   sd->sel.bottom = NULL;
   sd->sel.theme = NULL;
   sd->anim = NULL;
   sd->delayed_size_timer = NULL;
   sd->font.name = NULL;
   sd->pty = NULL;
   sd->imf = NULL;
   sd->win = NULL;
   sd->glayer = NULL;
   ecore_imf_shutdown();

   _parent_sc.del(obj);
}

static void
_smart_resize(Evas_Object *obj, Evas_Coord w, Evas_Coord h)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Coord ow, oh;

   EINA_SAFETY_ON_NULL_RETURN(sd);
   evas_object_geometry_get(obj, NULL, NULL, &ow, &oh);
   if ((ow == w) && (oh == h)) return;
   evas_object_smart_changed(obj);
   if (!sd->delayed_size_timer) sd->delayed_size_timer = 
     ecore_timer_add(0.0, _smart_cb_delayed_size, obj);
   else ecore_timer_delay(sd->delayed_size_timer, 0.0);
   evas_object_resize(sd->event, ow, oh);
}

static void
_smart_calculate(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Coord ox, oy, ow, oh;

   EINA_SAFETY_ON_NULL_RETURN(sd);

   evas_object_geometry_get(obj, &ox, &oy, &ow, &oh);
   evas_object_move(sd->grid.obj, ox, oy);
   evas_object_resize(sd->grid.obj,
                      sd->grid.w * sd->font.chw,
                      sd->grid.h * sd->font.chh);
   evas_object_move(sd->cursor.obj,
                    ox + (sd->cursor.x * sd->font.chw),
                    oy + (sd->cursor.y * sd->font.chh));
   evas_object_move(sd->event, ox, oy);
   evas_object_resize(sd->event, ow, oh);
}

static void
_smart_move(Evas_Object *obj, Evas_Coord x EINA_UNUSED, Evas_Coord y EINA_UNUSED)
{
   Termio *sd = evas_object_smart_data_get(obj);

   EINA_SAFETY_ON_NULL_RETURN(sd);
   evas_object_smart_changed(obj);
}

static void
_smart_init(void)
{
   static Evas_Smart_Class sc;

   evas_object_smart_clipped_smart_set(&_parent_sc);
   sc           = _parent_sc;
   sc.name      = "termio";
   sc.version   = EVAS_SMART_CLASS_VERSION;
   sc.add       = _smart_add;
   sc.del       = _smart_del;
   sc.resize    = _smart_resize;
   sc.move      = _smart_move;
   sc.calculate = _smart_calculate;
   _smart = evas_smart_class_new(&sc);
}

static void
_smart_pty_change(void *data)
{
   Termio *sd = evas_object_smart_data_get(data);
   EINA_SAFETY_ON_NULL_RETURN(sd);

// if scroll to bottom on updates
   if (sd->jump_on_change)  sd->scroll = 0;
   _smart_update_queue(data, sd);
}

void
termio_scroll(Evas_Object *obj, int direction, int start_y, int end_y)
{
   Termpty *ty;
   Termio *sd = evas_object_smart_data_get(obj);

   EINA_SAFETY_ON_NULL_RETURN(sd);

   if ((!sd->jump_on_change) && // if NOT scroll to bottom on updates
       (sd->scroll > 0))
     {
        // adjust scroll position for added scrollback
        sd->scroll++;
        if (sd->scroll > sd->pty->backscroll_num)
          sd->scroll = sd->pty->backscroll_num;
     }
   ty = sd->pty;
   if (ty->selection.is_active)
     {
        if (start_y <= ty->selection.start.y &&
            end_y >= ty->selection.end.y)
          {
             ty->selection.start.y += direction;
             ty->selection.end.y += direction;
             if (!(start_y <= ty->selection.start.y &&
                 end_y >= ty->selection.end.y))
               _sel_set(obj, EINA_FALSE);
          }
        else
          if (!((start_y > ty->selection.end.y) ||
                (end_y < ty->selection.start.y)))
            _sel_set(obj, EINA_FALSE);
     }
}

void
termio_content_change(Evas_Object *obj, Evas_Coord x, Evas_Coord y,
                      int n)
{
   Termpty *ty;
   int start_x, start_y, end_x, end_y;
   Termio *sd = evas_object_smart_data_get(obj);

   EINA_SAFETY_ON_NULL_RETURN(sd);
   ty = sd->pty;
   if (!ty->selection.is_active) return;

   start_x = sd->pty->selection.start.x;
   start_y = sd->pty->selection.start.y;
   end_x   = sd->pty->selection.end.x;
   end_y   = sd->pty->selection.end.y;
   if (ty->selection.is_box)
     {
        int _y = y + (x + n) / ty->w;

        if (start_y > end_y)
          INT_SWAP(start_y, end_y);
        if (start_x > end_x)
          INT_SWAP(start_x, end_x);

        y = MAX(y, start_y);
        for (; y <= MIN(_y, end_y); y++)
          {
             int d = MIN(n, ty->w - x);
             if (!((x > end_x) || (x + d < start_x)))
               {
                  _sel_set(obj, EINA_FALSE);
                  break;
               }
             n -= d;
             x = 0;
          }
     }
   else
     {
        int sel_len;
        Termcell *cells_changed, *cells_selection;

        /* probably doing that way too much… */
        if ((start_y > end_y) ||
            ((start_y == end_y) && (end_x < start_x)))
          {
             INT_SWAP(start_y, end_y);
             INT_SWAP(start_x, end_x);
          }

        sel_len = end_x - start_x + ty->w * (end_y - start_y);
        cells_changed = &(TERMPTY_SCREEN(ty, x, y));
        cells_selection = &(TERMPTY_SCREEN(ty, start_x, start_y));

        if (!((cells_changed > (cells_selection + sel_len)) ||
             (cells_selection > (cells_changed + n))))
          _sel_set(obj, EINA_FALSE);
     }
}

static void
_smart_pty_title(void *data)
{
   Evas_Object *obj = data;
   Termio *sd = evas_object_smart_data_get(obj);

   EINA_SAFETY_ON_NULL_RETURN(sd);
   if (!sd->win) return;
   evas_object_smart_callback_call(obj, "title,change", NULL);
//   elm_win_title_set(sd->win, sd->pty->prop.title);
}

static void
_smart_pty_icon(void *data)
{
   Evas_Object *obj = data;
   Termio *sd = evas_object_smart_data_get(obj);

   EINA_SAFETY_ON_NULL_RETURN(sd);
   if (!sd->win) return;
   evas_object_smart_callback_call(obj, "icon,change", NULL);
//   elm_win_icon_name_set(sd->win, sd->pty->prop.icon);
}

static void
_smart_pty_cancel_sel(void *data)
{
   Evas_Object *obj = data;
   Termio *sd = evas_object_smart_data_get(obj);

   EINA_SAFETY_ON_NULL_RETURN(sd);
   if (sd->pty->selection.is_active)
     {
        _sel_set(obj, EINA_FALSE);
        sd->pty->selection.makesel = EINA_FALSE;
        _smart_update_queue(data, sd);
     }
}

static void
_smart_pty_exited(void *data)
{
   evas_object_smart_callback_call(data, "exited", NULL);
}

static void
_smart_pty_bell(void *data)
{
   Termio *sd = evas_object_smart_data_get(data);

   EINA_SAFETY_ON_NULL_RETURN(sd);
   evas_object_smart_callback_call(data, "bell", NULL);
   edje_object_signal_emit(sd->cursor.obj, "bell", "terminology");
}

static void
_smart_pty_command(void *data)
{
   Evas_Object *obj = data;
   Termio *sd = evas_object_smart_data_get(obj);

   EINA_SAFETY_ON_NULL_RETURN(sd);
   if (!sd->pty->cur_cmd) return;
   if (sd->pty->cur_cmd[0] == 'i')
     {
        if ((sd->pty->cur_cmd[1] == 's') ||
            (sd->pty->cur_cmd[1] == 'c') ||
            (sd->pty->cur_cmd[1] == 'f') ||
            (sd->pty->cur_cmd[1] == 't') ||
            (sd->pty->cur_cmd[1] == 'j'))
          {
             const char *p, *p0, *p1, *path = NULL;
             char *pp;
             int ww = 0, hh = 0, repch;
             Eina_List *strs = NULL;
             
             // exact size in CHAR CELLS - WW (decimal) width CELLS,
             // HH (decimal) in CELLS.
             // 
             // isCWW;HH;PATH
             //  OR
             // isCWW;HH;LINK\nPATH
             //  OR specific to 'j' (edje)
             // ijCWW;HH;PATH\nGROUP[commands]
             //  WHERE [commands] is an optional string set of:
             // \nCMD\nP1[\nP2][\nP3][[\nCMD2\nP21[\nP22]]...
             //  CMD is the command, P1, P2, P3 etc. are parameters (P2 and
             //  on are optional depending on CMD)
             repch = sd->pty->cur_cmd[2];
             if (repch)
               {
                  char *link = NULL;
                  
                  for (p0 = p = &(sd->pty->cur_cmd[3]); *p; p++)
                    {
                       if (*p == ';')
                         {
                            ww = strtol(p0, NULL, 10);
                            p++;
                            break;
                         }
                    }
                  for (p0 = p; *p; p++)
                    {
                       if (*p == ';')
                         {
                            hh = strtol(p0, NULL, 10);
                            p++;
                            break;
                         }
                    }
                  if (sd->pty->cur_cmd[1] == 'j')
                    {
                       // parse from p until end of string - one newline
                       // per list item in strs
                       p0 = p1 = p;
                       for (;;)
                         {
                            // end of str param
                            if ((*p1 == '\n') || (*p1 == '\r') || (!*p1))
                              {
                                 // if string is non-empty...
                                 if ((p1 - p0) >= 1)
                                   {
                                      // allocate, fill and add to list
                                      pp = malloc(p1 - p0 + 1);
                                      if (pp)
                                        {
                                           strncpy(pp, p0, p1 - p0);
                                           pp[p1 - p0] = 0;
                                           strs = eina_list_append(strs, pp);
                                        }
                                   }
                                 // end of string buffer
                                 if (!*p1) break;
                                 p1++; // skip \n or \r
                                 p0 = p1;
                              }
                            else
                              p1++;
                         }
                    }
                  else
                    {
                       path = p;
                       p = strchr(path, '\n');
                       if (p)
                         {
                            link = strdup(path);
                            path = p + 1;
                            if (isspace(path[0])) path++;
                            pp = strchr(link, '\n');
                            if (pp) *pp = 0;
                            pp = strchr(link, '\r');
                            if (pp) *pp = 0;
                         }
                    }
                  if ((ww < 512) && (hh < 512))
                    {
                       Termblock *blk = NULL;

                       if (strs)
                         {
                            const char *file, *group;
                            Eina_List *l;
                            
                            file = eina_list_nth(strs, 0);
                            group = eina_list_nth(strs, 1);
                            l = eina_list_nth_list(strs, 2);
                            blk = termpty_block_new(sd->pty, ww, hh, file, group);
                            for (;l; l = l->next)
                              {
                                 pp = l->data;
                                 if (pp)
                                   blk->cmds = eina_list_append(blk->cmds, pp);
                                 l->data = NULL;
                              }
                         }
                       else
                         blk = termpty_block_new(sd->pty, ww, hh, path, link);
                       if (blk)
                         {
                            if (sd->pty->cur_cmd[1] == 's')
                              blk->scale_stretch = EINA_TRUE;
                            else if (sd->pty->cur_cmd[1] == 'c')
                              blk->scale_center = EINA_TRUE;
                            else if (sd->pty->cur_cmd[1] == 'f')
                              blk->scale_fill = EINA_TRUE;
                            else if (sd->pty->cur_cmd[1] == 't')
                              blk->thumb = EINA_TRUE;
                            else if (sd->pty->cur_cmd[1] == 'j')
                              blk->edje = EINA_TRUE;
                            termpty_block_insert(sd->pty, repch, blk);
                         }
                    }
                  if (link) free(link);
                  EINA_LIST_FREE(strs, pp) free(pp);
               }
             return;
          }
        else if (sd->pty->cur_cmd[1] == 'C')
          {
             Termblock *blk = NULL;
             const char *p, *p0, *p1;
             char *pp;
             Eina_List *strs = NULL;
             
             p = &(sd->pty->cur_cmd[2]);
             // parse from p until end of string - one newline
             // per list item in strs
             p0 = p1 = p;
             for (;;)
               {
                  // end of str param
                  if ((*p1 == '\n') || (*p1 == '\r') || (!*p1))
                    {
                       // if string is non-empty...
                       if ((p1 - p0) >= 1)
                         {
                            // allocate, fill and add to list
                            pp = malloc(p1 - p0 + 1);
                            if (pp)
                              {
                                 strncpy(pp, p0, p1 - p0);
                                 pp[p1 - p0] = 0;
                                 strs = eina_list_append(strs, pp);
                              }
                         }
                       // end of string buffer
                       if (!*p1) break;
                       p1++; // skip \n or \r
                       p0 = p1;
                    }
                  else
                    p1++;
               }
             if (strs)
               {
                  char *chid = strs->data;
                  blk = termpty_block_chid_get(sd->pty, chid);
                  if (blk)
                    _block_edje_cmds(sd->pty, blk, strs->next, EINA_FALSE);
               }
             EINA_LIST_FREE(strs, pp) free(pp);
          }
        else if (sd->pty->cur_cmd[1] == 'b')
          {
             sd->pty->block.on = EINA_TRUE;
          }
        else if (sd->pty->cur_cmd[1] == 'e')
          {
             sd->pty->block.on = EINA_FALSE;
          }
     }
   else if (sd->pty->cur_cmd[0] == 'q')
     {
        if (sd->pty->cur_cmd[1] == 's')
          {
             char buf[256];
             
             snprintf(buf, sizeof(buf), "%i;%i;%i;%i\n",
                      sd->grid.w, sd->grid.h, sd->font.chw, sd->font.chh);
             termpty_write(sd->pty, buf, strlen(buf));
             return;
          }
        else if (sd->pty->cur_cmd[1] == 'j')
          {
             const char *chid = &(sd->pty->cur_cmd[3]);
                  
             if (sd->pty->cur_cmd[2])
               {
                  if (sd->pty->cur_cmd[2] == '+')
                    {
                       sd->cur_chids = eina_list_append
                         (sd->cur_chids, eina_stringshare_add(chid));
                    }
                  else if (sd->pty->cur_cmd[2] == '-')
                    {
                       Eina_List *l;
                       char *chid2;
                       
                       EINA_LIST_FOREACH(sd->cur_chids, l, chid2)
                         {
                            if (!(!strcmp(chid, chid2)))
                              {
                                 sd->cur_chids =
                                   eina_list_remove_list(sd->cur_chids, l);
                                 eina_stringshare_del(chid2);
                                 break;
                              }
                         }
                    }
               }
             else
               {
                  EINA_LIST_FREE(sd->cur_chids, chid)
                    eina_stringshare_del(chid);
               }
             return;
          }
     }
   evas_object_smart_callback_call(obj, "command", (void *)sd->pty->cur_cmd);
}

#if !((ELM_VERSION_MAJOR == 1) && (ELM_VERSION_MINOR < 8))
static void
_smart_cb_drag_enter(void *data EINA_UNUSED, Evas_Object *o EINA_UNUSED)
{
   printf("dnd enter\n");
}

static void
_smart_cb_drag_leave(void *data EINA_UNUSED, Evas_Object *o EINA_UNUSED)
{
   printf("dnd leave\n");
}

static void
_smart_cb_drag_pos(void *data EINA_UNUSED, Evas_Object *o EINA_UNUSED, Evas_Coord x, Evas_Coord y, Elm_Xdnd_Action action)
{
   printf("dnd at %i %i act:%i\n", x, y, action);
}

static Eina_Bool
_smart_cb_drop(void *data, Evas_Object *o EINA_UNUSED, Elm_Selection_Data *ev)
{
   Evas_Object *obj = data;
   Termio *sd = evas_object_smart_data_get(obj);

   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, EINA_TRUE);
   if (ev->action == ELM_XDND_ACTION_COPY)
     {
        if (strchr(ev->data, '\n'))
          {
             char *p, *p2, *p3, *tb;
             
             tb = malloc(strlen(ev->data) + 1);
             if (tb)
               {
                  for (p = ev->data; p;)
                    {
                       p2 = strchr(p, '\n');
                       p3 = strchr(p, '\r');
                       if (p2 && p3)
                         {
                            if (p3 < p2) p2 = p3;
                         }
                       else if (!p2) p3 = p2;
                       if (p2)
                         {
                            strncpy(tb, p, p2 - p);
                            tb[p2 - p] = 0;
                            p = p2;
                            while ((*p) && (isspace(*p))) p++;
                            if (strlen(tb) > 0)
                              evas_object_smart_callback_call
                              (obj, "popup,queue", tb);
                         }
                       else
                         {
                            strcpy(tb, p);
                            if (strlen(tb) > 0)
                              evas_object_smart_callback_call
                              (obj, "popup,queue", tb);
                            break;
                         }
                    }
                  free(tb);
               }
          }
        else
          evas_object_smart_callback_call(obj, "popup", ev->data);
     }
   else
     termpty_write(sd->pty, ev->data, strlen(ev->data));
   return EINA_TRUE;
}
#endif

Evas_Object *
termio_add(Evas_Object *parent, Config *config, const char *cmd, Eina_Bool login_shell, const char *cd, int w, int h)
{
   Evas *e;
   Evas_Object *obj, *g;
   Termio *sd;

   EINA_SAFETY_ON_NULL_RETURN_VAL(parent, NULL);
   e = evas_object_evas_get(parent);
   if (!e) return NULL;

   if (!_smart) _smart_init();
   obj = evas_object_smart_add(e, _smart);
   sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, obj);

   termio_config_set(obj, config);

   sd->glayer = g = elm_gesture_layer_add(parent);
   elm_gesture_layer_attach(g, sd->event);

   elm_gesture_layer_cb_set(g, ELM_GESTURE_N_LONG_TAPS,
                            ELM_GESTURE_STATE_MOVE, _smart_cb_gest_long_move,
                            obj);
   
   elm_gesture_layer_cb_set(g, ELM_GESTURE_ZOOM,
                            ELM_GESTURE_STATE_START, _smart_cb_gest_zoom_start,
                            obj);
   elm_gesture_layer_cb_set(g, ELM_GESTURE_ZOOM,
                            ELM_GESTURE_STATE_MOVE, _smart_cb_gest_zoom_move,
                            obj);
   elm_gesture_layer_cb_set(g, ELM_GESTURE_ZOOM,
                            ELM_GESTURE_STATE_END, _smart_cb_gest_zoom_end,
                            obj);
   elm_gesture_layer_cb_set(g, ELM_GESTURE_ZOOM,
                            ELM_GESTURE_STATE_ABORT, _smart_cb_gest_zoom_abort,
                            obj);
   
#if !((ELM_VERSION_MAJOR == 1) && (ELM_VERSION_MINOR < 8))
   elm_drop_target_add(sd->event,
                       ELM_SEL_FORMAT_TEXT | ELM_SEL_FORMAT_IMAGE,
                       _smart_cb_drag_enter, obj,
                       _smart_cb_drag_leave, obj,
                       _smart_cb_drag_pos, obj,
                       _smart_cb_drop, obj);
#endif
   
   sd->pty = termpty_new(cmd, login_shell, cd, w, h, config->scrollback,
                         config->xterm_256color, config->erase_is_del);
   if (!sd->pty)
     {
        ERR("Cannot allocate termpty");
        evas_object_del(obj);
        return NULL;
     }
   sd->pty->obj = obj;
   sd->pty->cb.change.func = _smart_pty_change;
   sd->pty->cb.change.data = obj;
   sd->pty->cb.set_title.func = _smart_pty_title;
   sd->pty->cb.set_title.data = obj;
   sd->pty->cb.set_icon.func = _smart_pty_icon;
   sd->pty->cb.set_icon.data = obj;
   sd->pty->cb.cancel_sel.func = _smart_pty_cancel_sel;
   sd->pty->cb.cancel_sel.data = obj;
   sd->pty->cb.exited.func = _smart_pty_exited;
   sd->pty->cb.exited.data = obj;
   sd->pty->cb.bell.func = _smart_pty_bell;
   sd->pty->cb.bell.data = obj;
   sd->pty->cb.command.func = _smart_pty_command;
   sd->pty->cb.command.data = obj;
   _smart_size(obj, w, h, EINA_FALSE);
   return obj;
}

void
termio_win_set(Evas_Object *obj, Evas_Object *win)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN(sd);
   if (sd->win)
     {
        evas_object_event_callback_del_full(sd->win, EVAS_CALLBACK_DEL,
                                            _win_obj_del, obj);
        sd->win = NULL;
     }
   if (win)
     {
        sd->win = win;
        evas_object_event_callback_add(sd->win, EVAS_CALLBACK_DEL,
                                       _win_obj_del, obj);
     }
}

void
termio_theme_set(Evas_Object *obj, Evas_Object *theme)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN(sd);
   if (theme) sd->theme = theme;
}

Evas_Object *
termio_theme_get(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, NULL);
   return sd->theme;
}

char *
termio_selection_get(Evas_Object *obj, int c1x, int c1y, int c2x, int c2y,
                     size_t *len)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Eina_Strbuf *sb;
   char *s;
   int x, y;
   size_t len_backup;

   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, NULL);
   sb = eina_strbuf_new();
   termpty_cellcomp_freeze(sd->pty);
   for (y = c1y; y <= c2y; y++)
     {
        Termcell *cells;
        int w, last0, v, start_x, end_x;

        w = 0;
        last0 = -1;
        cells = termpty_cellrow_get(sd->pty, y, &w);
        if (!cells) continue;
        if (w > sd->grid.w) w = sd->grid.w;
        if (y == c1y && c1x >= w)
          {
             eina_strbuf_append_char(sb, '\n');
             continue;
          }
        start_x = c1x;
        end_x = (c2x >= w) ? w - 1 : c2x;
        if (c1y != c2y)
          {
             if (y == c1y) end_x = w - 1;
             else if (y == c2y) start_x = 0;
             else
               {
                  start_x = 0;
                  end_x = w - 1;
               }
          }
        for (x = start_x; x <= end_x; x++)
          {
#if defined(SUPPORT_DBLWIDTH)
             if ((cells[x].codepoint == 0) && (cells[x].att.dblwidth))
               {
                  if (x < end_x) x++;
                  else break;
               }
#endif
             if (x >= w) break;
             if ((cells[x].codepoint == 0) || (cells[x].codepoint == ' '))
               {
                  if (last0 < 0) last0 = x;
               }
             else if (cells[x].att.newline)
               {
                  last0 = -1;
                  if ((y != c2y) || (x != end_x))
                    eina_strbuf_append_char(sb, '\n');
                  break;
               }
             else if (cells[x].att.tab)
               {
                  eina_strbuf_append_char(sb, '\t');
                  x = ((x + 8) / 8) * 8;
                  x--;
               }
             else
               {
                  char txt[8];
                  int txtlen;

                  if (last0 >= 0)
                    {
                       v = x - last0 - 1;
                       last0 = -1;
                       while (v >= 0)
                         {
                            eina_strbuf_append_char(sb, ' ');
                            v--;
                         }
                    }
                  txtlen = codepoint_to_utf8(cells[x].codepoint, txt);
                  if (txtlen > 0)
                    eina_strbuf_append_length(sb, txt, txtlen);
                  if ((x == (w - 1)) && (x != c2x))
                    {
                       if (!cells[x].att.autowrapped)
                         eina_strbuf_append_char(sb, '\n');
                    }
               }
          }
        if (last0 >= 0)
          {
             if (y == c2y)
               {
                  Eina_Bool have_more = EINA_FALSE;

                  for (x = end_x + 1; x < w; x++)
                    {
#if defined(SUPPORT_DBLWIDTH)
                       if ((cells[x].codepoint == 0) &&
                           (cells[x].att.dblwidth))
                         {
                            if (x < (w - 1)) x++;
                            else break;
                         }
#endif
                       if (((cells[x].codepoint != 0) &&
                            (cells[x].codepoint != ' ')) ||
                           (cells[x].att.newline) ||
                           (cells[x].att.tab))
                         {
                            have_more = EINA_TRUE;
                            break;
                         }
                    }
                  if (!have_more) eina_strbuf_append_char(sb, '\n');
                  else
                    {
                       for (x = last0; x <= end_x; x++)
                         {
#if defined(SUPPORT_DBLWIDTH)
                            if ((cells[x].codepoint == 0) &&
                                (cells[x].att.dblwidth))
                              {
                                 if (x < (w - 1)) x++;
                                 else break;
                              }
#endif
                            if (x >= w) break;
                            eina_strbuf_append_char(sb, ' ');
                         }
                    }
               }
             else eina_strbuf_append_char(sb, '\n');
          }
     }
   termpty_cellcomp_thaw(sd->pty);

   if (!len) len = &len_backup;
   *len = eina_strbuf_length_get(sb);
   if (!*len)
     {
        eina_strbuf_free(sb);
        return NULL;
     }
   s = eina_strbuf_string_steal(sb);
   eina_strbuf_free(sb);
   return s;
}

void
termio_config_update(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Coord w, h;
   char buf[4096];

   EINA_SAFETY_ON_NULL_RETURN(sd);

   if (sd->font.name) eina_stringshare_del(sd->font.name);
   sd->font.name = NULL;

   if (sd->config->font.bitmap)
     {
        snprintf(buf, sizeof(buf), "%s/fonts/%s",
                 elm_app_data_dir_get(), sd->config->font.name);
        sd->font.name = eina_stringshare_add(buf);
     }
   else
     sd->font.name = eina_stringshare_add(sd->config->font.name);
   sd->font.size = sd->config->font.size;

   sd->jump_on_change = sd->config->jump_on_change;
   sd->jump_on_keypress = sd->config->jump_on_keypress;

   termpty_backscroll_set(sd->pty, sd->config->scrollback);
   sd->scroll = 0;

   if (evas_object_focus_get(obj))
     {
        edje_object_signal_emit(sd->cursor.obj, "focus,out", "terminology");
        if (sd->config->disable_cursor_blink)
          edje_object_signal_emit(sd->cursor.obj, "focus,in,noblink", "terminology");
        else
          edje_object_signal_emit(sd->cursor.obj, "focus,in", "terminology");
     }
   
   colors_term_init(sd->grid.obj, sd->theme, sd->config);
   
   evas_object_scale_set(sd->grid.obj, elm_config_scale_get());
   evas_object_textgrid_font_set(sd->grid.obj, sd->font.name, sd->font.size);
   evas_object_textgrid_cell_size_get(sd->grid.obj, &w, &h);
   if (w < 1) w = 1;
   if (h < 1) h = 1;
   sd->font.chw = w;
   sd->font.chh = h;
   _smart_size(obj, sd->grid.w, sd->grid.h, EINA_TRUE);
}

Config *
termio_config_get(const Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, NULL);
   return sd->config;
}

void
termio_copy_clipboard(Evas_Object *obj)
{
   _take_selection(obj, ELM_SEL_TYPE_CLIPBOARD);
}

void
termio_paste_clipboard(Evas_Object *obj)
{
   _paste_selection(obj, ELM_SEL_TYPE_CLIPBOARD);
}

const char  *
termio_link_get(const Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, NULL);
   return sd->link.string;
}

void
termio_mouseover_suspend_pushpop(Evas_Object *obj, int dir)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN(sd);
   sd->link.suspend += dir;
   if (sd->link.suspend < 0) sd->link.suspend = 0;
   if (sd->link.suspend)
     {
        if (sd->anim) ecore_animator_del(sd->anim);
        sd->anim = NULL;
        _smart_apply(obj);
     }
   else
     _smart_update_queue(obj, sd);
}

void
termio_event_feed_mouse_in(Evas_Object *obj)
{
   Evas *e;
   Termio *sd = evas_object_smart_data_get(obj);

   EINA_SAFETY_ON_NULL_RETURN(sd);
   e = evas_object_evas_get(obj);
   evas_event_feed_mouse_in(e, 0, NULL);
}

void
termio_size_get(Evas_Object *obj, int *w, int *h)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN(sd);
   if (w) *w = sd->grid.w;
   if (h) *h = sd->grid.h;
}

int
termio_scroll_get(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, 0);
   return sd->scroll;
}

pid_t
termio_pid_get(const Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, 0);
   return termpty_pid_get(sd->pty);
}

Eina_Bool
termio_cwd_get(const Evas_Object *obj, char *buf, size_t size)
{
   char procpath[PATH_MAX];
   Termio *sd = evas_object_smart_data_get(obj);
   pid_t pid;
   ssize_t siz;

   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, EINA_FALSE);

   pid = termpty_pid_get(sd->pty);
   snprintf(procpath, sizeof(procpath), "/proc/%d/cwd", pid);
   if ((siz = readlink(procpath, buf, size)) < 1)
     {
        ERR("Could not load working directory %s: %s",
            procpath, strerror(errno));
        return EINA_FALSE;
     }
   buf[siz] = 0;
   return EINA_TRUE;
}

Evas_Object *
termio_textgrid_get(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, NULL);

   return sd->grid.obj;
}

Evas_Object *
termio_win_get(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, NULL);

   return sd->win;
}


static void
_smart_mirror_del(void *data, Evas *evas EINA_UNUSED, Evas_Object *obj, void *info EINA_UNUSED)
{
   Termio *sd = evas_object_smart_data_get(data);
   EINA_SAFETY_ON_NULL_RETURN(sd);
   evas_object_event_callback_del_full(obj, EVAS_CALLBACK_DEL,
                                       _smart_mirror_del, data);
   sd->mirrors = eina_list_remove(sd->mirrors, obj);
}

Evas_Object *
termio_mirror_add(Evas_Object *obj)
{
   Evas_Object *img;
   Termio *sd = evas_object_smart_data_get(obj);
   Evas_Coord w = 0, h = 0;

   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, NULL);
   img = evas_object_image_filled_add(evas_object_evas_get(obj));
   evas_object_image_source_set(img, obj);
   evas_object_geometry_get(obj, NULL, NULL, &w, &h);
   evas_object_resize(img, w, h);
   sd->mirrors = eina_list_append(sd->mirrors, img);
   evas_object_data_set(img, "termio", obj);
   evas_object_event_callback_add(img, EVAS_CALLBACK_DEL,
                                  _smart_mirror_del, obj);
   return img;
}

const char *
termio_title_get(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, NULL);
   return sd->pty->prop.title;
}

const char *
termio_icon_name_get(Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, NULL);
   return sd->pty->prop.icon;
}

void
termio_debugwhite_set(Evas_Object *obj, Eina_Bool dbg)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN(sd);
   sd->debugwhite = dbg;
   _smart_apply(obj);
}

Eina_Bool
termio_selection_exists(const Evas_Object *obj)
{
   Termio *sd = evas_object_smart_data_get(obj);
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, EINA_FALSE);
   return sd->pty->selection.is_active;
}
