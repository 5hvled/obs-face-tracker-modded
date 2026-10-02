#include <obs-module.h>
#include <util/platform.h>

#include <util/threading.h>
#include <graphics/vec2.h>
#include <graphics/graphics.h>
#include "plugin-macros.generated.h"
#include "texture-object.h"
#include <algorithm>
#include <graphics/matrix4.h>
#include "helper.hpp"
#include "face-tracker.hpp"
#include "face-tracker-preset.h"
#include "face-tracker-manager.hpp"
#include "source_list.h"

extern "C" uint64_t os_gettime_ns(void);


static inline void scale_texture(struct face_tracker_filter *s, float scale);
static inline int stage_to_surface(struct face_tracker_filter *s, float scale);
static inline std::shared_ptr<texture_object> surface_to_cvtex(struct face_tracker_filter *s, float scale);

class ft_manager_for_ftf : public face_tracker_manager {
public:
	struct face_tracker_filter *ctx;

public:
	ft_manager_for_ftf(struct face_tracker_filter *ctx_) { ctx = ctx_; }

	~ft_manager_for_ftf() { release_cvtex(); }

	inline void release_cvtex() {}

	std::shared_ptr<texture_object> get_cvtex() override
	{
		if (scale < 1.0f)
			scale = 1.0f;
		scale_texture(ctx, scale);
		if (stage_to_surface(ctx, scale))
			return NULL;
		return surface_to_cvtex(ctx, scale);
	};
};

static const char *ftf_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Face Tracker");
}

static inline void get_aspect_from_str(struct face_tracker_filter *s, const char *str)
{
	if (sscanf(str, "%d:%d", &s->aspect_x, &s->aspect_y) == 2)
		return;
	if (sscanf(str, "%dx%d", &s->aspect_x, &s->aspect_y) == 2)
		return;
	s->aspect_x = 0;
	s->aspect_y = 0;
}

static void load_prop_texture(struct face_tracker_filter *s, const char *path)
{
	if (s->prop_path && path && strcmp(s->prop_path, path) == 0)
		return;

	obs_enter_graphics();
	if (s->prop_texture) {
		gs_texture_destroy(s->prop_texture);
		s->prop_texture = NULL;
	}
	if (path && *path)
		s->prop_texture = gs_texture_create_from_file(path);
	obs_leave_graphics();
        bfree(s->prop_path);
        s->prop_path = bstrdup(path ? path : "");
}

static void ftf_update(void *data, obs_data_t *settings)
{
	auto *s = (struct face_tracker_filter *)data;

	s->ftm->update(settings);
	s->track_z = obs_data_get_double(settings, "track_z");
	s->track_x = obs_data_get_double(settings, "track_x");
	s->track_y = obs_data_get_double(settings, "track_y");
	s->scale_max = obs_data_get_double(settings, "scale_max");


	s->prop_enabled = obs_data_get_bool(settings, "prop_enabled");

	s->prop_scale = (float)obs_data_get_double(settings, "prop_scale");
	s->prop_offset_x = (float)obs_data_get_double(settings, "prop_offset_x");
	s->prop_offset_y = (float)obs_data_get_double(settings, "prop_offset_y");
	s->prop_opacity = (float)obs_data_get_double(settings, "prop_opacity") * 0.01f;
	s->prop_pos_smoothing = (float)obs_data_get_double(settings, "prop_pos_smoothing");
	s->prop_scale_smoothing = (float)obs_data_get_double(settings, "prop_scale_smoothing");
	s->prop_rotation_smoothing = (float)obs_data_get_double(settings, "prop_rotation_smoothing");
	s->prop_rotation = (float)obs_data_get_double(settings, "prop_rotation");
	s->prop_min_size = (float)obs_data_get_double(settings, "prop_min_size");
	s->prop_max_size = (float)obs_data_get_double(settings, "prop_max_size");
	s->prop_max_rotation = (float)obs_data_get_double(settings, "prop_max_rotation") * 0.01745329252f;
	s->prop_anchor = (int)obs_data_get_int(settings, "prop_anchor");
	s->prop_lost_behavior = (int)obs_data_get_int(settings, "prop_lost_behavior");
	s->prop_lost_fade_time = (float)obs_data_get_double(settings, "prop_lost_fade_time");
	s->prop_follow_size = obs_data_get_bool(settings, "prop_follow_size");
	s->prop_follow_rotation = obs_data_get_bool(settings, "prop_follow_rotation");
	s->prop_hide_lost = obs_data_get_bool(settings, "prop_hide_lost");
	s->prop_3d_enabled = obs_data_get_bool(settings, "prop_3d_enabled");
	s->prop_3d_follow_yaw = obs_data_get_bool(settings, "prop_3d_follow_yaw");
	s->prop_3d_follow_pitch = obs_data_get_bool(settings, "prop_3d_follow_pitch");
	s->prop_3d_follow_roll = obs_data_get_bool(settings, "prop_3d_follow_roll");
	s->prop_3d_yaw_amount = (float)obs_data_get_double(settings, "prop_3d_yaw_amount");
	s->prop_3d_pitch_amount = (float)obs_data_get_double(settings, "prop_3d_pitch_amount");
	s->prop_3d_roll_amount = (float)obs_data_get_double(settings, "prop_3d_roll_amount");
	s->prop_3d_yaw_smoothing = (float)obs_data_get_double(settings, "prop_3d_yaw_smoothing");
	s->prop_3d_pitch_smoothing = (float)obs_data_get_double(settings, "prop_3d_pitch_smoothing");
	s->prop_3d_roll_smoothing = (float)obs_data_get_double(settings, "prop_3d_roll_smoothing");
	load_prop_texture(s, obs_data_get_string(settings, "prop_path"));

        s->tracked_source_enabled = obs_data_get_bool(settings, "tracked_source_enabled");
        const char *tracked_name = obs_data_get_string(settings, "tracked_source_name");
        bfree(s->tracked_source_name);
        s->tracked_source_name = bstrdup(tracked_name ? tracked_name : "");

        if (s->tracked_source_ref) {
                obs_weak_source_release(s->tracked_source_ref);
                s->tracked_source_ref = NULL;
        }

        if (s->tracked_source_enabled && s->tracked_source_name && *s->tracked_source_name) {
                obs_source_t *tracked_source = obs_get_source_by_name(s->tracked_source_name);
                if (tracked_source && tracked_source != s->context) {
                        s->tracked_source_ref = obs_source_get_weak_source(tracked_source);
                        obs_source_release(tracked_source);
                } else if (tracked_source) {
                        blog(LOG_WARNING, "Face Tracker: refusing to track itself");
                        obs_source_release(tracked_source);
                }
        }

        s->tracked_source_scale = (float)obs_data_get_double(settings, "tracked_source_scale");
        s->tracked_source_offset_x = (float)obs_data_get_double(settings, "tracked_source_offset_x");
        s->tracked_source_offset_y = (float)obs_data_get_double(settings, "tracked_source_offset_y");
        s->tracked_source_opacity = (float)obs_data_get_double(settings, "tracked_source_opacity") * 0.01f;
        s->tracked_source_pos_smoothing = (float)obs_data_get_double(settings, "tracked_source_pos_smoothing");
        s->tracked_source_scale_smoothing = (float)obs_data_get_double(settings, "tracked_source_scale_smoothing");
        s->tracked_source_rotation_smoothing = (float)obs_data_get_double(settings, "tracked_source_rotation_smoothing");
        s->tracked_source_rotation = (float)obs_data_get_double(settings, "tracked_source_rotation");
        s->tracked_source_min_size = (float)obs_data_get_double(settings, "tracked_source_min_size");
        s->tracked_source_max_size = (float)obs_data_get_double(settings, "tracked_source_max_size");
        s->tracked_source_max_rotation = (float)obs_data_get_double(settings, "tracked_source_max_rotation") * 0.01745329252f;
        s->tracked_source_lost_behavior = (int)obs_data_get_int(settings, "tracked_source_lost_behavior");
        s->tracked_source_lost_fade_time = (float)obs_data_get_double(settings, "tracked_source_lost_fade_time");
        s->tracked_source_follow_size = obs_data_get_bool(settings, "tracked_source_follow_size");
        s->tracked_source_follow_rotation = obs_data_get_bool(settings, "tracked_source_follow_rotation");
        s->tracked_source_3d_enabled = obs_data_get_bool(settings, "tracked_source_3d_enabled");
        s->tracked_source_3d_follow_yaw = obs_data_get_bool(settings, "tracked_source_3d_follow_yaw");
        s->tracked_source_3d_follow_pitch = obs_data_get_bool(settings, "tracked_source_3d_follow_pitch");
        s->tracked_source_3d_follow_roll = obs_data_get_bool(settings, "tracked_source_3d_follow_roll");
        s->tracked_source_3d_yaw_amount = (float)obs_data_get_double(settings, "tracked_source_3d_yaw_amount");
        s->tracked_source_3d_pitch_amount = (float)obs_data_get_double(settings, "tracked_source_3d_pitch_amount");
        s->tracked_source_3d_roll_amount = (float)obs_data_get_double(settings, "tracked_source_3d_roll_amount");
        s->tracked_source_3d_yaw_smoothing = (float)obs_data_get_double(settings, "tracked_source_3d_yaw_smoothing");
        s->tracked_source_3d_pitch_smoothing = (float)obs_data_get_double(settings, "tracked_source_3d_pitch_smoothing");
        s->tracked_source_3d_roll_smoothing = (float)obs_data_get_double(settings, "tracked_source_3d_roll_smoothing");

        s->face_size_trigger_enabled = obs_data_get_bool(settings, "face_size_trigger_enabled");
        s->face_size_trigger_min = (float)obs_data_get_double(settings, "face_size_trigger_min") * 0.01f;
        s->face_size_trigger_max = (float)obs_data_get_double(settings, "face_size_trigger_max") * 0.01f;

	double kp = obs_data_get_double(settings, "Kp");
	float ki = (float)obs_data_get_double(settings, "Ki");
	double td = obs_data_get_double(settings, "Td");
	double att2 = from_dB(obs_data_get_double(settings, "att2_dB"));
	s->kp.v[0] = s->kp.v[1] = (float)kp;
	s->kp.v[2] = (float)(att2 * kp);
	s->ki = ki;
	s->klpf = s->kp * td;
	s->tlpf.v[0] = s->tlpf.v[1] = (float)obs_data_get_double(settings, "Tdlpf");
	s->tlpf.v[2] = (float)obs_data_get_double(settings, "Tdlpf_z");
	s->e_deadband.v[0] = (float)obs_data_get_double(settings, "e_deadband_x") * 1e-2;
	s->e_deadband.v[1] = (float)obs_data_get_double(settings, "e_deadband_y") * 1e-2;
	s->e_deadband.v[2] = (float)obs_data_get_double(settings, "e_deadband_z") * 1e-2;
	s->e_nonlinear.v[0] = (float)obs_data_get_double(settings, "e_nonlinear_x") * 1e-2;
	s->e_nonlinear.v[1] = (float)obs_data_get_double(settings, "e_nonlinear_y") * 1e-2;
	s->e_nonlinear.v[2] = (float)obs_data_get_double(settings, "e_nonlinear_z") * 1e-2;

	get_aspect_from_str(s, obs_data_get_string(settings, "aspect"));

	s->inactive_reset = obs_data_get_bool(settings, "inactive_reset");

	s->debug_faces = obs_data_get_bool(settings, "debug_faces");
	s->debug_notrack = obs_data_get_bool(settings, "debug_notrack");
	s->debug_always_show = obs_data_get_bool(settings, "debug_always_show");

	debug_data_open(&s->debug_data_tracker, &s->debug_data_tracker_last, settings, "debug_data_tracker");
	debug_data_open(&s->debug_data_error, &s->debug_data_error_last, settings, "debug_data_error");
	debug_data_open(&s->debug_data_control, &s->debug_data_control_last, settings, "debug_data_control");
}

static void fts_update(void *data, obs_data_t *settings)
{
        auto *s = (struct face_tracker_filter *)data;
        ftf_update(data, settings);

        const char *target_name = obs_data_get_string(settings, "target_name");
 if (target_name && *target_name) {
 bfree(s->target_name);
 s->target_name = bstrdup(target_name);
 }
}

static void cb_render_frame(void *data, calldata_t *cd);
static void cb_render_info(void *data, calldata_t *cd);
static void cb_get_target_size(void *data, calldata_t *cd);
static void cb_get_state(void *data, calldata_t *cd);
static void cb_set_state(void *data, calldata_t *cd);
static const char *ftptz_signals[] = {
	"void state_changed()",
	"void face_detected(bool active, int face_id)",
	"void face_lost(bool active, int face_id)",
	"void face_size_trigger(bool active, int face_id)",
	NULL
};
static void emit_state_changed(struct face_tracker_filter *);
static void emit_face_event(struct face_tracker_filter *, const char *signal, bool active, int face_id);

static void *ftf_create(obs_data_t *settings, obs_source_t *context)
{
	auto *s = (struct face_tracker_filter *)bzalloc(sizeof(struct face_tracker_filter));
	s->ftm = new ft_manager_for_ftf(s);
	s->ftm->crop_cur.x1 = s->ftm->crop_cur.y1 = -2;
	s->context = context;
	s->ftm->scale = 2.0f;
	s->hotkey_pause = OBS_INVALID_HOTKEY_PAIR_ID;
	s->hotkey_reset = OBS_INVALID_HOTKEY_ID;
	s->prop_size = 120.0f;
	s->prop_tracking = false;
	s->tracked_source_tracking = false;
	s->selected_face_id = 0;
	s->prop_render_opacity = 1.0f;
	s->tracked_source_render_opacity = 1.0f;
	s->pose_3d_reference_set = false;
	s->pose_3d_current_yaw = s->pose_3d_current_pitch = s->pose_3d_current_roll = 0.0f;
	s->pose_3d_reference_yaw = s->pose_3d_reference_pitch = s->pose_3d_reference_roll = 0.0f;
	s->face_selection_mode = 0;

	obs_source_update(context, settings);

	proc_handler_t *ph = obs_source_get_proc_handler(context);
	proc_handler_add(ph, "void render_frame(bool notrack)", cb_render_frame, s);
	proc_handler_add(ph, "void render_info(bool notrack)", cb_render_info, s);
	proc_handler_add(ph, "void get_target_size(out int width, out int height)", cb_get_target_size, s);
	proc_handler_add(ph, "void get_state()", cb_get_state, s);
	proc_handler_add(ph, "void set_state()", cb_set_state, s);

	signal_handler_t *sh = obs_source_get_signal_handler(context);
	signal_handler_add_array(sh, ftptz_signals);

	return s;
}

static void register_hotkeys(struct face_tracker_filter *s, obs_source_t *target);

static void *fts_create(obs_data_t *settings, obs_source_t *context)
{
	auto *s = (struct face_tracker_filter *)ftf_create(settings, context);

	register_hotkeys(s, context);

	return s;
}

static void ftf_destroy(void *data)
{
	auto *s = (struct face_tracker_filter *)data;

	if (s->hotkey_pause != OBS_INVALID_HOTKEY_PAIR_ID)
		obs_hotkey_pair_unregister(s->hotkey_pause);
	if (s->hotkey_reset != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(s->hotkey_reset);

	obs_enter_graphics();
	gs_texrender_destroy(s->texrender);
        gs_texrender_destroy(s->tracked_texrender);
        s->tracked_texrender = NULL;
	s->texrender = NULL;
	gs_texrender_destroy(s->texrender_scaled);
	s->texrender_scaled = NULL;
	gs_stagesurface_destroy(s->stagesurface);
	s->stagesurface = NULL;
	gs_texture_destroy(s->prop_texture);
	s->prop_texture = NULL;
	obs_leave_graphics();

	delete s->ftm;

	bfree(s->target_name);
	bfree(s->prop_path);
        bfree(s->tracked_source_name);
        obs_weak_source_release(s->tracked_source_ref);
	obs_weak_source_release(s->target_ref);
	if (s->debug_data_tracker)
		fclose(s->debug_data_tracker);
	if (s->debug_data_error)
		fclose(s->debug_data_error);
	if (s->debug_data_control)
		fclose(s->debug_data_control);
	bfree(s->debug_data_tracker_last);
	bfree(s->debug_data_error_last);
	bfree(s->debug_data_control_last);

	bfree(s);
}

static bool ftf_set_3d_reference(obs_properties_t *, obs_property_t *, void *data)
{
	auto *s = (struct face_tracker_filter *)data;

	// The latest pose is continuously updated while a face is tracked.
	// Store it as the new neutral orientation and immediately zero both
	// overlay pose outputs so the change takes effect on the next frame.
	s->pose_3d_reference_yaw = s->pose_3d_current_yaw;
	s->pose_3d_reference_pitch = s->pose_3d_current_pitch;
	s->pose_3d_reference_roll = s->pose_3d_current_roll;
	s->pose_3d_reference_set = true;

	s->prop_3d_yaw = 0.0f;
	s->prop_3d_pitch = 0.0f;
	s->prop_3d_roll = 0.0f;
	s->tracked_source_3d_yaw = 0.0f;
	s->tracked_source_3d_pitch = 0.0f;
	s->tracked_source_3d_roll = 0.0f;

	return true;
}

static bool ftf_reset_tracking(obs_properties_t *, obs_property_t *, void *data)
{
	auto *s = (struct face_tracker_filter *)data;

	float w = s->known_width;
	float h = s->known_height;
	float z = sqrtf(s->width_with_aspect * s->height_with_aspect);
	s->detect_err = f3(0, 0, 0);
	s->filter_int_out = f3(w * 0.5f, h * 0.5f, z);
	s->filter_int = f3(0, 0, 0);
	s->filter_lpf = f3(0, 0, 0);
	s->ftm->reset_requested = true;

	return true;
}

static obs_properties_t *ftf_properties(void *data)
{
	auto *s = (struct face_tracker_filter *)data;
	obs_properties_t *props;
	props = obs_properties_create();

	{
		obs_properties_t *info = obs_properties_create();
		obs_properties_add_text(info, "plugin_author", "Creator: @5hvled", OBS_TEXT_INFO);
		obs_properties_add_text(info, "plugin_version", "Version: v2.1.2-stage3-multiface", OBS_TEXT_INFO);
		obs_properties_add_text(info, "plugin_branch", "Build branch: stage3-multiface", OBS_TEXT_INFO);
		obs_properties_add_group(props, "plugin_info", "5hvled Face Tracker", OBS_GROUP_NORMAL, info);
	}

	{
		obs_properties_t *fp = obs_properties_create();
		obs_property_t *face_sel = obs_properties_add_list(fp, "face_selection_mode", "Tracked face",
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(face_sel, "Primary face (highest confidence)", 0);
		obs_property_list_add_int(face_sel, "Face 1", 1);
		obs_property_list_add_int(face_sel, "Face 2", 2);
		obs_property_list_add_int(face_sel, "Largest face", 3);
		obs_property_list_add_int(face_sel, "All faces", 4);
		obs_properties_add_group(props, "face_selection", "Face selection", OBS_GROUP_NORMAL, fp);
	}

	obs_properties_add_button(props, "ftf_reset_tracking", obs_module_text("Reset tracking"), ftf_reset_tracking);

	{
		obs_properties_t *pp = obs_properties_create();
		obs_property_t *p = obs_properties_add_list(pp, "preset_name", obs_module_text("Preset"),
							    OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
		obs_data_t *settings = obs_source_get_settings(s->context);
		if (settings) {
			ftf_preset_item_to_list(p, settings);
			obs_data_release(settings);
		}
		obs_properties_add_button(pp, "preset_load", obs_module_text("Load preset"), ftf_preset_load);
		obs_properties_add_button(pp, "preset_save", obs_module_text("Save preset"), ftf_preset_save);
		obs_properties_add_button(pp, "preset_delete", obs_module_text("Delete preset"), ftf_preset_delete);
		obs_properties_add_bool(pp, "preset_mask_track", obs_module_text("Save and load tracking parameters"));
		obs_properties_add_bool(pp, "preset_mask_control", obs_module_text("Save and load control parameters"));
		obs_properties_add_group(props, "preset_grp", obs_module_text("Preset"), OBS_GROUP_NORMAL, pp);
	}

	{
		obs_properties_t *pp = obs_properties_create();
		face_tracker_manager::get_properties(pp);
		obs_properties_add_group(props, "ftm", obs_module_text("Face detection options"), OBS_GROUP_NORMAL, pp);
	}

	{
		obs_properties_t *pp = obs_properties_create();
		obs_properties_add_float(pp, "track_z", obs_module_text("Zoom"), 0.1, 2.0, 0.05);
		obs_properties_add_float(pp, "track_x", obs_module_text("X"), -1.0, +1.0, 0.05);
		obs_properties_add_float(pp, "track_y", obs_module_text("Y"), -1.0, +1.0, 0.05);
		obs_properties_add_float(pp, "scale_max", obs_module_text("Scale max"), 1.0, 20.0, 1.0);
		obs_properties_add_group(props, "track", obs_module_text("Tracking target location"), OBS_GROUP_NORMAL,
					 pp);
	}

{
		obs_properties_t *pp = obs_properties_create();
		obs_properties_add_bool(pp, "prop_enabled", "Enable Face Prop");
		obs_properties_add_path(pp, "prop_path", "PNG image", OBS_PATH_FILE, "PNG files (*.png);;All files (*.*)", NULL);
		obs_properties_add_float(pp, "prop_scale", "Prop scale", 0.1, 10.0, 0.05);
		obs_properties_add_float(pp, "prop_offset_x", "X offset (face widths)", -3.0, 3.0, 0.05);
		obs_properties_add_float(pp, "prop_offset_y", "Y offset (face widths)", -3.0, 3.0, 0.05);
 obs_properties_add_float_slider(pp, "prop_rotation", "Rotation (degrees)", -180.0, 180.0, 1.0);
		obs_properties_add_float_slider(pp, "prop_opacity", "Opacity", 0.0, 100.0, 1.0);
		obs_properties_add_bool(pp, "prop_follow_size", "Follow face size");
		obs_properties_add_bool(pp, "prop_follow_rotation", "Follow head tilt");
		obs_properties_add_bool(pp, "prop_hide_lost", "Hide when face is lost");
		obs_property_t *anchor=obs_properties_add_list(pp,"prop_anchor","Prop anchor",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(anchor,"Face center",0);
		obs_property_list_add_int(anchor,"Left eye",1);
		obs_property_list_add_int(anchor,"Right eye",2);
		obs_property_list_add_int(anchor,"Between eyes",3);
		obs_property_list_add_int(anchor,"Nose",4);
		obs_properties_add_float_slider(pp,"prop_pos_smoothing","Position smoothing",0.0,0.99,0.01);
		obs_properties_add_float_slider(pp,"prop_scale_smoothing","Scale smoothing",0.0,0.99,0.01);
		obs_properties_add_float_slider(pp,"prop_rotation_smoothing","Rotation smoothing",0.0,0.99,0.01);
		obs_properties_add_float(pp,"prop_min_size","Minimum face size",1.0,10000.0,1.0);
		obs_properties_add_float(pp,"prop_max_size","Maximum face size",1.0,10000.0,1.0);
		obs_properties_add_float_slider(pp,"prop_max_rotation","Max head rotation",0.0,180.0,1.0);
		obs_property_t *lost=obs_properties_add_list(pp,"prop_lost_behavior","When face is lost",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(lost,"Hide",0);
		obs_property_list_add_int(lost,"Freeze",1);
		obs_property_list_add_int(lost,"Fade out",2);
		obs_properties_add_float(pp,"prop_lost_fade_time","Fade time (seconds)",0.05,5.0,0.05);
		obs_properties_add_button(pp, "prop_3d_set_reference", "Set Current Head Pose as 3D Reference", ftf_set_3d_reference);
		obs_properties_add_bool(pp, "prop_3d_enabled", "Enable 3D Head Tracking");
		obs_properties_add_bool(pp, "prop_3d_follow_yaw", "Follow Yaw (turn left/right)");
		obs_properties_add_bool(pp, "prop_3d_follow_pitch", "Follow Pitch (look up/down)");
		obs_properties_add_bool(pp, "prop_3d_follow_roll", "Follow Roll (tilt head)");
		obs_properties_add_float_slider(pp, "prop_3d_yaw_amount", "3D Yaw amount", 0.0, 2.0, 0.05);
		obs_properties_add_float_slider(pp, "prop_3d_pitch_amount", "3D Pitch amount", 0.0, 2.0, 0.05);
		obs_properties_add_float_slider(pp, "prop_3d_roll_amount", "3D Roll amount", 0.0, 2.0, 0.05);
		obs_properties_add_float_slider(pp, "prop_3d_yaw_smoothing", "3D Yaw smoothing", 0.0, 0.99, 0.01);
		obs_properties_add_float_slider(pp, "prop_3d_pitch_smoothing", "3D Pitch smoothing", 0.0, 0.99, 0.01);
		obs_properties_add_float_slider(pp, "prop_3d_roll_smoothing", "3D Roll smoothing", 0.0, 0.99, 0.01);
		obs_properties_add_group(props, "face_prop", "Face Prop Overlay", OBS_GROUP_NORMAL, pp);
                obs_properties_t *sp=obs_properties_create();
                obs_properties_add_bool(sp, "tracked_source_enabled", "Enable Tracked Source / Scene");
                obs_property_t *tracked_p = obs_properties_add_list(sp, "tracked_source_name", "Tracked Source / Scene", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
                property_list_add_sources(tracked_p, s ? s->context : NULL);
                obs_properties_add_float(sp,"tracked_source_scale","Source scale",0.1,10.0,0.05);
                obs_properties_add_float(sp,"tracked_source_offset_x","X offset (face widths)",-3.0,3.0,0.05);
                obs_properties_add_float(sp,"tracked_source_offset_y","Y offset (face widths)",-3.0,3.0,0.05);
                obs_properties_add_float_slider(sp,"tracked_source_opacity","Opacity",0.0,100.0,1.0);
                obs_properties_add_float_slider(sp,"tracked_source_pos_smoothing","Position smoothing",0.0,0.99,0.01);
                obs_properties_add_float_slider(sp,"tracked_source_scale_smoothing","Scale smoothing",0.0,0.99,0.01);
                obs_properties_add_float_slider(sp,"tracked_source_rotation_smoothing","Rotation smoothing",0.0,0.99,0.01);
                obs_properties_add_float_slider(sp,"tracked_source_rotation","Rotation (degrees)",-180.0,180.0,1.0);
                obs_properties_add_bool(sp,"tracked_source_follow_size","Follow face size");
                obs_properties_add_bool(sp,"tracked_source_follow_rotation","Follow head tilt");
                obs_properties_add_float(sp,"tracked_source_min_size","Minimum face size",1.0,10000.0,1.0);
                obs_properties_add_float(sp,"tracked_source_max_size","Maximum face size",1.0,10000.0,1.0);
                obs_properties_add_float_slider(sp,"tracked_source_max_rotation","Max head rotation",0.0,180.0,1.0);
                obs_properties_add_button(sp,"tracked_source_3d_set_reference","Set Current Head Pose as 3D Reference",ftf_set_3d_reference);
                obs_properties_add_bool(sp,"tracked_source_3d_enabled","Enable 3D Head Tracking");
                obs_properties_add_bool(sp,"tracked_source_3d_follow_yaw","Follow Yaw (turn left/right)");
                obs_properties_add_bool(sp,"tracked_source_3d_follow_pitch","Follow Pitch (look up/down)");
                obs_properties_add_bool(sp,"tracked_source_3d_follow_roll","Follow Roll (tilt head)");
                obs_properties_add_float_slider(sp,"tracked_source_3d_yaw_amount","3D Yaw amount",0.0,2.0,0.05);
                obs_properties_add_float_slider(sp,"tracked_source_3d_pitch_amount","3D Pitch amount",0.0,2.0,0.05);
                obs_properties_add_float_slider(sp,"tracked_source_3d_roll_amount","3D Roll amount",0.0,2.0,0.05);
                obs_properties_add_float_slider(sp,"tracked_source_3d_yaw_smoothing","3D Yaw smoothing",0.0,0.99,0.01);
                obs_properties_add_float_slider(sp,"tracked_source_3d_pitch_smoothing","3D Pitch smoothing",0.0,0.99,0.01);
                obs_properties_add_float_slider(sp,"tracked_source_3d_roll_smoothing","3D Roll smoothing",0.0,0.99,0.01);
                obs_property_t *slost=obs_properties_add_list(sp,"tracked_source_lost_behavior","When face is lost",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
                obs_property_list_add_int(slost,"Hide",0);
                obs_property_list_add_int(slost,"Freeze",1);
                obs_property_list_add_int(slost,"Fade out",2);
                obs_properties_add_float(sp,"tracked_source_lost_fade_time","Fade time (seconds)",0.05,5.0,0.05);
                obs_properties_add_group(props,"tracked_source","Tracked Source / Scene Settings",OBS_GROUP_NORMAL,sp);
	}

	{
		obs_properties_t *pp = obs_properties_create();
		obs_properties_add_float(pp, "Kp", "Track Kp", 0.01, 10.0, 0.1);
		obs_properties_add_float(pp, "Ki", "Track Ki", 0.0, 5.0, 0.01);
		obs_properties_add_float(pp, "Td", "Track Td", 0.0, 5.0, 0.01);
		obs_properties_add_float(pp, "Tdlpf", "Track LPF for Td (X, Y)", 0.0, 10.0, 0.1);
		obs_properties_add_float(pp, "Tdlpf_z", "Track LPF for Td (Z)", 0.0, 10.0, 0.1);
		obs_properties_add_float(pp, "att2_dB", "Attenuation (Z)", -60.0, 10.0, 1.0);
		obs_properties_add_float(pp, "e_deadband_x", "Dead band (X)", 0.0, 50, 0.1);
		obs_properties_add_float(pp, "e_deadband_y", "Dead band (Y)", 0.0, 50, 0.1);
		obs_properties_add_float(pp, "e_deadband_z", "Dead band (Z)", 0.0, 50, 0.1);
		obs_properties_add_float(pp, "e_nonlinear_x", "Nonlinear band (X)", 0.0, 50, 0.1);
		obs_properties_add_float(pp, "e_nonlinear_y", "Nonlinear band (Y)", 0.0, 50, 0.1);
		obs_properties_add_float(pp, "e_nonlinear_z", "Nonlinear band (Z)", 0.0, 50, 0.1);
		obs_properties_add_group(props, "ctrl", obs_module_text("Tracking response"), OBS_GROUP_NORMAL, pp);
	}

	{
		obs_properties_t *pp = obs_properties_create();
		obs_property_t *p = obs_properties_add_list(pp, "aspect", obs_module_text("Aspect"),
							    OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
		const char *aspects[] = {"16:9", "4:3", "1:1", "3:4", "9:16", NULL};
		obs_property_list_add_string(p, obs_module_text("same as the source"), "");
		for (int i = 0; aspects[i]; i++)
			obs_property_list_add_string(p, aspects[i], aspects[i]);
		obs_properties_add_group(props, "output", obs_module_text("Output"), OBS_GROUP_NORMAL, pp);
	}

	{
		obs_properties_t *pp = obs_properties_create();
		obs_properties_add_bool(pp, "inactive_reset", obs_module_text("Prop.Automation.InactiveReset"));
		obs_properties_add_group(props, "automation", obs_module_text("Automation"), OBS_GROUP_NORMAL, pp);
	}

	{
		obs_properties_t *pp=obs_properties_create();
		obs_properties_add_bool(pp,"face_size_trigger_enabled","Enable face size trigger");
		obs_properties_add_float_slider(pp,"face_size_trigger_min","Minimum face size (%)",0.0,100.0,1.0);
		obs_properties_add_float_slider(pp,"face_size_trigger_max","Maximum face size (%)",0.0,100.0,1.0);
		obs_properties_add_group(props,"face_events","Face Events",OBS_GROUP_NORMAL,pp);
	}

	{
		obs_properties_t *pp = obs_properties_create();
		obs_properties_add_bool(pp, "debug_faces", "Show face detection results");
		obs_properties_add_bool(pp, "debug_notrack", "Stop tracking faces");
		obs_properties_add_bool(pp, "debug_always_show", "Always show information (useful for demo)");
#ifdef ENABLE_DEBUG_DATA
		obs_properties_add_path(pp, "debug_data_tracker", "Save correlation tracker data to file",
					OBS_PATH_FILE_SAVE, DEBUG_DATA_PATH_FILTER, NULL);
		obs_properties_add_path(pp, "debug_data_error", "Save calculated error data to file",
					OBS_PATH_FILE_SAVE, DEBUG_DATA_PATH_FILTER, NULL);
		obs_properties_add_path(pp, "debug_data_control", "Save control data to file", OBS_PATH_FILE_SAVE,
					DEBUG_DATA_PATH_FILTER, NULL);
#endif
		obs_properties_add_group(props, "debug", obs_module_text("Debugging"), OBS_GROUP_NORMAL, pp);
	}

	return props;
}

static obs_properties_t *fts_properties(void *data)
{
	auto *s = (struct face_tracker_filter *)data;

	obs_properties_t *props = ftf_properties(data);

	obs_properties_t *pp = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(pp, "target_name", obs_module_text("Source"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	property_list_add_sources(p, s ? s->context : NULL);

	obs_properties_add_group(props, "input", obs_module_text("Input"), OBS_GROUP_NORMAL, pp);

	return props;
}

static void ftf_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "preset_mask_track", true);
	obs_data_set_default_bool(settings, "preset_mask_control", true);
	face_tracker_manager::get_defaults(settings);
	obs_data_set_default_double(settings, "track_z", 0.70);  //  1.00  0.50  0.35
	obs_data_set_default_double(settings, "track_y", +0.00); // +0.00 +0.10 +0.30
	obs_data_set_default_double(settings, "scale_max", 10.0);

	obs_data_set_default_int(settings, "face_selection_mode", 0);
	obs_data_set_default_bool(settings, "prop_enabled", false);
	obs_data_set_default_string(settings, "prop_path", "");
	obs_data_set_default_double(settings, "prop_scale", 2.2);
	obs_data_set_default_double(settings, "prop_offset_x", 0.0);
	obs_data_set_default_double(settings, "prop_offset_y", -0.10);
	obs_data_set_default_double(settings, "prop_rotation", 0.0);
	obs_data_set_default_double(settings, "prop_opacity", 100.0);
	obs_data_set_default_double(settings, "prop_pos_smoothing", 0.70);
	obs_data_set_default_double(settings, "prop_scale_smoothing", 0.75);
	obs_data_set_default_double(settings, "prop_rotation_smoothing", 0.85);
	obs_data_set_default_double(settings, "prop_min_size", 1.0);
	obs_data_set_default_double(settings, "prop_max_size", 10000.0);
	obs_data_set_default_double(settings, "prop_max_rotation", 180.0);
	obs_data_set_default_int(settings, "prop_anchor", 0);
	obs_data_set_default_int(settings, "prop_lost_behavior", 0);
	obs_data_set_default_double(settings, "prop_lost_fade_time", 0.25);
	obs_data_set_default_bool(settings, "prop_follow_size", true);
	obs_data_set_default_bool(settings, "prop_follow_rotation", true);
	obs_data_set_default_bool(settings, "prop_hide_lost", true);
	obs_data_set_default_bool(settings, "prop_3d_enabled", false);
	obs_data_set_default_bool(settings, "prop_3d_follow_yaw", true);
	obs_data_set_default_bool(settings, "prop_3d_follow_pitch", true);
	obs_data_set_default_bool(settings, "prop_3d_follow_roll", true);
	obs_data_set_default_double(settings, "prop_3d_yaw_amount", 1.0);
	obs_data_set_default_double(settings, "prop_3d_pitch_amount", 1.0);
	obs_data_set_default_double(settings, "prop_3d_roll_amount", 1.0);
	obs_data_set_default_double(settings, "prop_3d_yaw_smoothing", 0.75);
	obs_data_set_default_double(settings, "prop_3d_pitch_smoothing", 0.75);
	obs_data_set_default_double(settings, "prop_3d_roll_smoothing", 0.75);
	obs_data_set_default_bool(settings, "tracked_source_enabled", false);
	obs_data_set_default_string(settings, "tracked_source_name", "");
	obs_data_set_default_double(settings, "tracked_source_scale", 2.2);
	obs_data_set_default_double(settings, "tracked_source_offset_x", 0.0);
	obs_data_set_default_double(settings, "tracked_source_offset_y", -0.10);
	obs_data_set_default_double(settings, "tracked_source_opacity", 100.0);
	obs_data_set_default_double(settings, "tracked_source_pos_smoothing", 0.70);
	obs_data_set_default_double(settings, "tracked_source_scale_smoothing", 0.75);
	obs_data_set_default_double(settings, "tracked_source_rotation_smoothing", 0.85);
	obs_data_set_default_double(settings, "tracked_source_rotation", 0.0);
	obs_data_set_default_double(settings, "tracked_source_min_size", 1.0);
	obs_data_set_default_double(settings, "tracked_source_max_size", 10000.0);
	obs_data_set_default_double(settings, "tracked_source_max_rotation", 180.0);
	obs_data_set_default_int(settings, "tracked_source_lost_behavior", 0);
	obs_data_set_default_double(settings, "tracked_source_lost_fade_time", 0.25);
	obs_data_set_default_bool(settings, "tracked_source_follow_size", true);
	obs_data_set_default_bool(settings, "tracked_source_follow_rotation", true);
	obs_data_set_default_bool(settings, "tracked_source_3d_enabled", false);
	obs_data_set_default_bool(settings, "tracked_source_3d_follow_yaw", true);
	obs_data_set_default_bool(settings, "tracked_source_3d_follow_pitch", true);
	obs_data_set_default_bool(settings, "tracked_source_3d_follow_roll", true);
	obs_data_set_default_double(settings, "tracked_source_3d_yaw_amount", 1.0);
	obs_data_set_default_double(settings, "tracked_source_3d_pitch_amount", 1.0);
	obs_data_set_default_double(settings, "tracked_source_3d_roll_amount", 1.0);
	obs_data_set_default_double(settings, "tracked_source_3d_yaw_smoothing", 0.75);
	obs_data_set_default_double(settings, "tracked_source_3d_pitch_smoothing", 0.75);
	obs_data_set_default_double(settings, "tracked_source_3d_roll_smoothing", 0.75);
	obs_data_set_default_bool(settings, "face_size_trigger_enabled", false);
	obs_data_set_default_double(settings, "face_size_trigger_min", 0.0);
	obs_data_set_default_double(settings, "face_size_trigger_max", 100.0);

	obs_data_set_default_double(settings, "Kp", 0.95);
	obs_data_set_default_double(settings, "Ki", 0.3);
	obs_data_set_default_double(settings, "Td", 0.42);
	obs_data_set_default_double(settings, "Tdlpf", 2.0);
	obs_data_set_default_double(settings, "Tdlpf_z", 6.0);
	obs_data_set_default_double(settings, "att2_dB", -10);

	obs_data_t *presets = obs_data_create();
	obs_data_set_default_obj(settings, "presets", presets);
	obs_data_release(presets);
}

static void tick_filter(struct face_tracker_filter *s, float second)
{
	const float srwh = sqrtf((float)s->known_width * s->known_height);

	f3 e = s->detect_err;
	f3 e_int = e;
	for (int i = 0; i < 3; i++) {
		float x = e.v[i];
		float d = srwh * s->e_deadband.v[i];
		float n = srwh * s->e_nonlinear.v[i];
		if (std::abs(x) <= d)
			x = 0.0f;
		else if (std::abs(x) < (d + n)) {
			if (x > 0)
				x = +sqf(x - d) / (2.0f * n);
			else
				x = -sqf(x - d) / (2.0f * n);
		} else if (x > 0)
			x -= d + n * 0.5f;
		else
			x += d + n * 0.5f;
		if (second * s->ki > 1.0e-10) {
			if (s->filter_int.v[i] < 0.0f && e.v[i] > 0.0f)
				e_int.v[i] = std::min(e.v[i], -s->filter_int.v[i] / (second * s->ki));
			else if (s->filter_int.v[i] > 0.0f && e.v[i] < 0.0f)
				e_int.v[i] = std::max(e.v[i], -s->filter_int.v[i] / (second * s->ki));
			else
				e_int.v[i] = x;
		}
		e.v[i] = x;
	}

	s->filter_int_out += (e + s->filter_int).hp(s->kp * second);
	s->filter_int += e_int * (second * s->ki);
	for (int i = 0; i < 3; i++)
		s->filter_lpf.v[i] = (s->filter_lpf.v[i] * s->tlpf.v[i] + e.v[i] * second) / (s->tlpf.v[i] + second);

	f3 u = s->filter_int_out + s->filter_lpf.hp(s->klpf);

	for (int i = 0; i < 3; i++) {
		if (isnan(u.v[i]))
			u.v[i] = (s->range_max.v[i] + s->range_min_out.v[i]) * 0.5f;
		else if (u.v[i] < s->range_min_out.v[i]) {
			u.v[i] = s->range_min_out.v[i];
			if (s->filter_int_out.v[i] < s->range_min_out.v[i])
				s->filter_int_out.v[i] = s->range_min_out.v[i];
		} else if (u.v[i] > s->range_max.v[i]) {
			u.v[i] = s->range_max.v[i];
			if (s->filter_int_out.v[i] > s->range_max.v[i])
				s->filter_int_out.v[i] = s->range_max.v[i];
		}
	}

	s->u_last = u;

	if (s->debug_data_control) {
		fprintf(s->debug_data_control, "%f\t%f\t%f\t%f\n", os_gettime_ns() * 1e-9, u.v[0], u.v[1], u.v[2]);
	}

	s->ftm->crop_cur = f3_to_rectf(u, s->width_with_aspect, s->height_with_aspect);
}

static void ftf_activate(void *data)
{
	auto *s = (struct face_tracker_filter *)data;
	s->is_active = true;
}

static void ftf_deactivate(void *data)
{
	auto *s = (struct face_tracker_filter *)data;
	s->is_active = false;

	if (s->inactive_reset) {
		ftf_reset_tracking(nullptr, nullptr, data);
		s->ftm->crop_cur = f3_to_rectf(s->filter_int_out, s->width_with_aspect, s->height_with_aspect);
	}
}

static inline void calculate_overlay_target(const face_tracker_manager::tracker_rect_s &tr, int anchor,
	float &x, float &y, float &size, float &angle)
{
	x = (tr.rect.x0 + tr.rect.x1) * 0.5f;
	y = (tr.rect.y0 + tr.rect.y1) * 0.5f;
	size = get_width(tr.rect);
	angle = 0.0f;
	if (tr.landmark.empty())
		return;
	pointf_s center = landmark_center(tr.landmark);
	float area = landmark_area(tr.landmark);
	if (area > 0.0f)
		size = sqrtf(area * (float)(4.0f / M_PI));
	if (tr.landmark.size() >= 5) {
		float lx = (tr.landmark[0].x + tr.landmark[1].x) * 0.5f;
		float ly = (tr.landmark[0].y + tr.landmark[1].y) * 0.5f;
		float rx = (tr.landmark[2].x + tr.landmark[3].x) * 0.5f;
		float ry = (tr.landmark[2].y + tr.landmark[3].y) * 0.5f;
		switch (anchor) {
		case 1: x = lx; y = ly; break;
		case 2: x = rx; y = ry; break;
		case 3: x = (lx + rx) * 0.5f; y = (ly + ry) * 0.5f; break;
		case 4: x = tr.landmark[4].x; y = tr.landmark[4].y; break;
		default: x = center.x; y = center.y; break;
		}
		angle = atan2f(ry - ly, rx - lx);
	} else {
		x = center.x; y = center.y;
	}
}

static inline void calculate_head_pose_3d(const face_tracker_manager::tracker_rect_s &tr,
	float &yaw, float &pitch, float &roll)
{
	yaw = pitch = roll = 0.0f;
	if (tr.landmark.size() < 5)
		return;

	const float lx = (tr.landmark[0].x + tr.landmark[1].x) * 0.5f;
	const float ly = (tr.landmark[0].y + tr.landmark[1].y) * 0.5f;
	const float rx = (tr.landmark[2].x + tr.landmark[3].x) * 0.5f;
	const float ry = (tr.landmark[2].y + tr.landmark[3].y) * 0.5f;
	const float nx = tr.landmark[4].x;
	const float ny = tr.landmark[4].y;
	const float ex = rx - lx;
	const float ey = ry - ly;
	const float eye_dist = sqrtf(ex * ex + ey * ey);
	if (eye_dist < 1.0f)
		return;

	const float eye_angle = atan2f(ey, ex);
	const float c = cosf(eye_angle);
	const float sn = sinf(eye_angle);
	const float mx = (lx + rx) * 0.5f;
	const float my = (ly + ry) * 0.5f;
	const float nose_x = (nx - mx) * c + (ny - my) * sn;
	const float nose_y = -(nx - mx) * sn + (ny - my) * c;

	roll = eye_angle;
	const float yaw_norm = nose_x / eye_dist;
	const float pitch_norm = (nose_y / eye_dist) - 0.58f;
	yaw = atan2f(yaw_norm, 0.55f);
	pitch = -atan2f(pitch_norm, 1.10f);

	const float max_pose = 75.0f * 0.01745329252f;
	yaw = std::max(-max_pose, std::min(max_pose, yaw));
	pitch = std::max(-max_pose, std::min(max_pose, pitch));
	roll = std::max(-max_pose, std::min(max_pose, roll));
}

static inline float lost_alpha(bool tracking, int behavior, float fade_time, float elapsed, float base)
{
	if (tracking || behavior == 1)
		return base;
	if (behavior == 2 && fade_time > 0.0f)
		return base * std::max(0.0f, 1.0f - elapsed / fade_time);
	return 0.0f;
}


static inline void calculate_error(struct face_tracker_filter *s);

static const face_tracker_manager::tracker_rect_s *select_tracking_face(
	const struct face_tracker_filter *s,
	const std::vector<face_tracker_manager::tracker_rect_s> &tracker_rects)
{
	std::vector<const face_tracker_manager::tracker_rect_s *> valid_faces;
	for (const auto &tr : tracker_rects) {
		if (tr.rect.score > 0.0f && tr.face_id > 0)
			valid_faces.push_back(&tr);
	}

	if (valid_faces.empty())
		return nullptr;

	if (s->face_selection_mode == 0) {
		return *std::max_element(valid_faces.begin(), valid_faces.end(),
			[](const auto *a, const auto *b) {
				return a->rect.score < b->rect.score;
			});
	}

	// "All faces" is a rendering mode. Camera tracking still needs one
	// deterministic face, so use the primary face for the camera crop while
	// the prop/source render paths draw every detected face.
	if (s->face_selection_mode == 4)
		return *std::max_element(valid_faces.begin(), valid_faces.end(),
			[](const auto *a, const auto *b) {
				return a->rect.score < b->rect.score;
			});

	if (s->face_selection_mode == 3) {
		return *std::max_element(valid_faces.begin(), valid_faces.end(),
			[](const auto *a, const auto *b) {
				const int aa = (a->rect.x1 - a->rect.x0) * (a->rect.y1 - a->rect.y0);
				const int ab = (b->rect.x1 - b->rect.x0) * (b->rect.y1 - b->rect.y0);
				return aa < ab;
			});
	}

	std::sort(valid_faces.begin(), valid_faces.end(),
		[](const auto *a, const auto *b) {
			return a->face_id < b->face_id;
		});

	const size_t requested = (size_t)(s->face_selection_mode - 1);
	return requested < valid_faces.size() ? valid_faces[requested] : nullptr;
}

static void calculate_aspect(struct face_tracker_filter *s)
{
	if (s->aspect_y <= 0 || s->aspect_x <= 0) {
		s->width_with_aspect = s->known_width;
		s->height_with_aspect = s->known_height;
	} else if (s->known_width * s->aspect_y >= s->known_height * s->aspect_x) {
		s->height_with_aspect = s->known_height;
		s->width_with_aspect = s->aspect_x * s->known_height / s->aspect_y;
	} else {
		s->width_with_aspect = s->known_width;
		s->height_with_aspect = s->known_width * s->aspect_y / s->aspect_x;
	}
}

static bool hotkey_cb_pause(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	auto *s = (struct face_tracker_filter *)data;
	if (!pressed)
		return false;
	if (s->is_paused)
		return false;
	s->is_paused = true;
	emit_state_changed(s);
	return true;
}

static bool hotkey_cb_pause_resume(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	auto *s = (struct face_tracker_filter *)data;
	if (!pressed)
		return false;
	if (!s->is_paused)
		return false;
	s->is_paused = false;
	emit_state_changed(s);
	return true;
}

static void hotkey_cb_reset(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		ftf_reset_tracking(NULL, NULL, data);
}

static void register_hotkeys(struct face_tracker_filter *s, obs_source_t *target)
{
	if (!target)
		return;

	if (s->hotkey_pause == OBS_INVALID_HOTKEY_PAIR_ID) {
		s->hotkey_pause = obs_hotkey_pair_register_source(target, "face-tracker.pause",
								  obs_module_text("Pause Face Tracker"),
								  "face-tracker.pause_resume",
								  obs_module_text("Resume Face Tracker"),
								  hotkey_cb_pause, hotkey_cb_pause_resume, s, s);
	}

	if (s->hotkey_reset == OBS_INVALID_HOTKEY_ID) {
		s->hotkey_reset = obs_hotkey_register_source(target, "face-tracker.reset",
							     obs_module_text("Reset Face Tracker"), hotkey_cb_reset, s);
	}
}

inline static bool is_running(struct face_tracker_filter *s)
{
	if (s->is_paused)
		return false;

	if (s->inactive_reset && !s->is_active)
		return false;

	return true;
}

static void ft_tick_internal(struct face_tracker_filter *s, float second, bool was_rendered)
{
	if (s->known_width <= 0 || s->known_height <= 0) {
		return;
	}

	calculate_aspect(s);

	if (s->ftm->crop_cur.x1 < -1 || s->ftm->crop_cur.y1 < -1) {
		ftf_reset_tracking(NULL, NULL, s);
		s->ftm->crop_cur = f3_to_rectf(s->filter_int_out, s->width_with_aspect, s->height_with_aspect);
	} else if (was_rendered && !s->is_paused) {
		s->range_min.v[0] = get_width(s->ftm->crop_cur) * 0.5f;
		s->range_max.v[0] = s->known_width - get_width(s->ftm->crop_cur) * 0.5f;
		s->range_min.v[1] = get_height(s->ftm->crop_cur) * 0.5f;
		s->range_max.v[1] = s->known_height - get_height(s->ftm->crop_cur) * 0.5f;
		s->range_min.v[2] = sqrtf(s->known_width * s->known_height) / s->scale_max;
		s->range_max.v[2] = sqrtf(s->width_with_aspect * s->height_with_aspect);
		s->range_min_out = s->range_min;
		s->range_min_out.v[2] = std::max(std::min(s->range_min.v[2], s->u_last.v[2]), 1.0f);
		calculate_error(s);
		if (s->prop_tracking) {
			s->prop_lost_elapsed=0.0f;
			s->prop_render_opacity=s->prop_opacity;
			s->tracked_source_lost_elapsed=0.0f;
			s->tracked_source_render_opacity=s->tracked_source_opacity;
		} else {
			s->prop_lost_elapsed+=second;
			s->tracked_source_lost_elapsed+=second;
			s->prop_render_opacity=lost_alpha(false,s->prop_lost_behavior,s->prop_lost_fade_time,s->prop_lost_elapsed,s->prop_opacity);
			s->tracked_source_render_opacity=lost_alpha(false,s->tracked_source_lost_behavior,s->tracked_source_lost_fade_time,s->tracked_source_lost_elapsed,s->tracked_source_opacity);
		}
		if (s->face_size_trigger_enabled) {
			float frame_size = sqrtf((float)s->known_width * s->known_height);
			float face_size = 0.0f;
			if (s->selected_face_id > 0) {
				for (const auto &tr : s->ftm->tracker_rects) {
					if (tr.face_id == s->selected_face_id && tr.rect.score > 0.0f) {
						face_size = get_width(tr.rect);
						break;
					}
				}
			}
			float ratio = frame_size > 0.0f ? face_size / frame_size : 0.0f;
			bool active = s->selected_face_id > 0 &&
				ratio >= s->face_size_trigger_min && ratio <= s->face_size_trigger_max;
			if (active != s->face_size_triggered) {
				s->face_size_triggered = active;
				emit_face_event(s, "face_size_trigger", active, s->selected_face_id);
			}
		}
		tick_filter(s, second);
	}

	s->target_valid = true;
}

static void ftf_tick(void *data, float second)
{
	auto *s = (struct face_tracker_filter *)data;
	const bool was_rendered = s->rendered;
	s->rendered = false;
	s->target_valid = false;

	s->ftm->tick(second);

	obs_source_t *target = obs_filter_get_target(s->context);
	if (!target)
		return;

	if (s->hotkey_pause == OBS_INVALID_HOTKEY_PAIR_ID || s->hotkey_reset == OBS_INVALID_HOTKEY_ID)
		register_hotkeys(s, obs_filter_get_parent(s->context));

	s->known_width = obs_source_get_base_width(target);
	s->known_height = obs_source_get_base_height(target);

	ft_tick_internal(s, second, was_rendered);
}

static void fts_tick(void *data, float second)
{
	auto *s = (struct face_tracker_filter *)data;
	const bool was_rendered = s->rendered;
	s->rendered = false;
	s->target_valid = false;

	s->ftm->tick(second);

	obs_source_t *target = obs_weak_source_get_source(s->target_ref);
	const char *name = target ? obs_source_get_name(target) : NULL;

	if (s->target_name && (!target || !name || strcmp(name, s->target_name))) {
		obs_source_release(target);
		obs_weak_source_release(s->target_ref);
		target = obs_get_source_by_name(s->target_name);
		s->target_ref = obs_source_get_weak_source(target);
		blog(LOG_INFO, "fts_tick: target=%p", target);
	}

	s->known_width = obs_source_get_width(target);
	s->known_height = obs_source_get_height(target);

	ft_tick_internal(s, second, was_rendered);

	obs_source_release(target);
}

static inline void render_target(struct face_tracker_filter *s, obs_source_t *target, obs_source_t *parent)
{
	if (!s->texrender)
		s->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	const uint32_t cx = s->known_width, cy = s->known_height;
	gs_texrender_reset(s->texrender);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(s->texrender, cx, cy)) {
		uint32_t parent_flags = obs_source_get_output_flags(target);
		bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		struct vec4 clear_color;

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

		if (target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);

		gs_texrender_end(s->texrender);
	}

	gs_blend_state_pop();

	s->ftm->release_cvtex();
}

static inline f3 ensure_range(f3 u, const struct face_tracker_filter *s)
{
	if (isnan(u.v[2]))
		u.v[2] = s->range_min.v[2];
	else if (u.v[2] < s->range_min.v[2])
		u.v[2] = s->range_min.v[2];
	else if (u.v[2] > s->range_max.v[2])
		u.v[2] = s->range_max.v[2];

	rectf_s r = f3_to_rectf(u, s->width_with_aspect, s->height_with_aspect);

	if (r.x0 < 0)
		u.v[0] += -r.x0;
	else if (r.x1 > s->known_width)
		u.v[0] -= r.x1 - s->known_width;

	if (r.y0 < 0)
		u.v[1] += -r.y0;
	else if (r.y1 > s->known_height)
		u.v[1] -= r.y1 - s->known_height;

	return u;
}

static inline void calculate_error(struct face_tracker_filter *s)
{
	f3 e_tot(0.0f, 0.0f, 0.0f);
	float sc_tot = 0.0f;
	bool found = false;
	auto &tracker_rects = s->ftm->tracker_rects;
	const face_tracker_manager::tracker_rect_s *best = select_tracking_face(s, tracker_rects);

	if (best) {
		f3 r(best->rect);
		float score = best->rect.score;

		if (s->ftm->landmark_detection_data) {
			pointf_s center = landmark_center(best->landmark);
			float area = landmark_area(best->landmark);
			if (area > 0.0f) {
				r.v[0] = center.x;
				r.v[1] = center.y;
				r.v[2] = sqrtf(area * (float)(4.0f / M_PI));
			}
		}

		if (s->debug_data_tracker) {
			fprintf(s->debug_data_tracker, "%f\t%f\t%f\t%f\t%f\n",
				os_gettime_ns() * 1e-9, r.v[0], r.v[1], r.v[2], score);
		}

		r.v[0] -= get_width(best->crop_rect) * s->track_x;
		r.v[1] += get_height(best->crop_rect) * s->track_y;
		r.v[2] /= s->track_z;
		r = ensure_range(r, s);
		f3 w(best->crop_rect);
		f3 e = (r - w) * score;

		if (score > 0.0f && !isnan(e)) {
			e_tot = e;
			sc_tot = score;
			found = true;
		}
	}

	if (found)
		s->detect_err = e_tot * (1.0f / sc_tot);
	else
		s->detect_err = f3(0, 0, 0);

	// The same selected face drives camera tracking, props, and tracked sources.
	const bool was_tracking = s->prop_tracking;
	const int previous_face_id = s->selected_face_id;
	s->selected_face_id = best ? best->face_id : 0;
	if (best) {
		float tx, ty, ts, ta;
		float pose_yaw = 0.0f, pose_pitch = 0.0f, pose_roll = 0.0f;
		calculate_head_pose_3d(*best, pose_yaw, pose_pitch, pose_roll);

		// Keep the raw camera pose so the reference button can capture it.
		s->pose_3d_current_yaw = pose_yaw;
		s->pose_3d_current_pitch = pose_pitch;
		s->pose_3d_current_roll = pose_roll;

		// Once a reference is set, all 3D motion is measured relative to it.
		if (s->pose_3d_reference_set) {
			pose_yaw -= s->pose_3d_reference_yaw;
			pose_pitch -= s->pose_3d_reference_pitch;
			pose_roll -= s->pose_3d_reference_roll;
			while (pose_roll > (float)M_PI) pose_roll -= (float)(2 * M_PI);
			while (pose_roll < -(float)M_PI) pose_roll += (float)(2 * M_PI);
		}

		calculate_overlay_target(*best, s->prop_anchor, tx, ty, ts, ta);
		ts = std::max(s->prop_min_size, std::min(s->prop_max_size, ts));
		ta = std::max(-s->prop_max_rotation, std::min(s->prop_max_rotation, ta));
		float pa = 1.0f - std::max(0.0f, std::min(0.99f, s->prop_pos_smoothing));
		float sa = 1.0f - std::max(0.0f, std::min(0.99f, s->prop_scale_smoothing));
		float ra = 1.0f - std::max(0.0f, std::min(0.99f, s->prop_rotation_smoothing));
		if (!s->prop_tracking) {
			s->prop_x=tx; s->prop_y=ty; s->prop_size=ts; s->prop_angle=ta;
		} else {
			s->prop_x += (tx-s->prop_x)*pa;
			s->prop_y += (ty-s->prop_y)*pa;
			s->prop_size += (ts-s->prop_size)*sa;
			float da=ta-s->prop_angle;
			while(da>(float)M_PI) da-=(float)(2*M_PI);
			while(da<-(float)M_PI) da+=(float)(2*M_PI);
			s->prop_angle += da*ra;
		}
		const bool prop_was_tracking = s->prop_tracking;
		s->prop_tracking=true;
		if (s->prop_3d_enabled) {
			const float ya = pose_yaw * s->prop_3d_yaw_amount;
			const float pi = pose_pitch * s->prop_3d_pitch_amount;
			const float ro = pose_roll * s->prop_3d_roll_amount;
			const float yk = 1.0f - std::max(0.0f, std::min(0.99f, s->prop_3d_yaw_smoothing));
			const float pk = 1.0f - std::max(0.0f, std::min(0.99f, s->prop_3d_pitch_smoothing));
			const float rk = 1.0f - std::max(0.0f, std::min(0.99f, s->prop_3d_roll_smoothing));
			if (!prop_was_tracking) {
				s->prop_3d_yaw = ya; s->prop_3d_pitch = pi; s->prop_3d_roll = ro;
			} else {
				s->prop_3d_yaw += (ya - s->prop_3d_yaw) * yk;
				s->prop_3d_pitch += (pi - s->prop_3d_pitch) * pk;
				s->prop_3d_roll += (ro - s->prop_3d_roll) * rk;
			}
		} else {
			s->prop_3d_yaw = s->prop_3d_pitch = s->prop_3d_roll = 0.0f;
		}

		if (s->tracked_source_enabled) {
			float x,y,sz,ang;
			calculate_overlay_target(*best, 0, x,y,sz,ang);
			sz=std::max(s->tracked_source_min_size,std::min(s->tracked_source_max_size,sz));
			ang=std::max(-s->tracked_source_max_rotation,std::min(s->tracked_source_max_rotation,ang));
			float p=1.0f-std::max(0.0f,std::min(0.99f,s->tracked_source_pos_smoothing));
			float sc=1.0f-std::max(0.0f,std::min(0.99f,s->tracked_source_scale_smoothing));
			float r=1.0f-std::max(0.0f,std::min(0.99f,s->tracked_source_rotation_smoothing));
			if(!s->tracked_source_tracking){
				s->tracked_source_x=x;s->tracked_source_y=y;s->tracked_source_size=sz;s->tracked_source_angle=ang;
			}else{
				s->tracked_source_x+=(x-s->tracked_source_x)*p;
				s->tracked_source_y+=(y-s->tracked_source_y)*p;
				s->tracked_source_size+=(sz-s->tracked_source_size)*sc;
				float da=ang-s->tracked_source_angle;
				while(da>(float)M_PI)da-=(float)(2*M_PI);
				while(da<-(float)M_PI)da+=(float)(2*M_PI);
				s->tracked_source_angle+=da*r;
			}
			const bool tracked_was_tracking = s->tracked_source_tracking;
			s->tracked_source_tracking=true;
			if (s->tracked_source_3d_enabled) {
				const float ya = pose_yaw * s->tracked_source_3d_yaw_amount;
				const float pi = pose_pitch * s->tracked_source_3d_pitch_amount;
				const float ro = pose_roll * s->tracked_source_3d_roll_amount;
				const float yk = 1.0f - std::max(0.0f, std::min(0.99f, s->tracked_source_3d_yaw_smoothing));
				const float pk = 1.0f - std::max(0.0f, std::min(0.99f, s->tracked_source_3d_pitch_smoothing));
				const float rk = 1.0f - std::max(0.0f, std::min(0.99f, s->tracked_source_3d_roll_smoothing));
				if (!tracked_was_tracking) {
					s->tracked_source_3d_yaw = ya; s->tracked_source_3d_pitch = pi; s->tracked_source_3d_roll = ro;
				} else {
					s->tracked_source_3d_yaw += (ya - s->tracked_source_3d_yaw) * yk;
					s->tracked_source_3d_pitch += (pi - s->tracked_source_3d_pitch) * pk;
					s->tracked_source_3d_roll += (ro - s->tracked_source_3d_roll) * rk;
				}
			} else {
				s->tracked_source_3d_yaw = s->tracked_source_3d_pitch = s->tracked_source_3d_roll = 0.0f;
			}
		}
	} else {
		s->prop_tracking=false;
		s->tracked_source_tracking=false;
	}
	if (was_tracking && !s->prop_tracking) {
		emit_face_event(s, "face_lost", false, previous_face_id);
	} else if (!was_tracking && s->prop_tracking) {
		emit_face_event(s, "face_detected", true, s->selected_face_id);
	} else if (was_tracking && s->prop_tracking && previous_face_id != s->selected_face_id) {
		emit_face_event(s, "face_lost", false, previous_face_id);
		emit_face_event(s, "face_detected", true, s->selected_face_id);
	}

	if (s->debug_data_error) {
		fprintf(s->debug_data_error, "%f\t%f\t%f\t%f\n", os_gettime_ns() * 1e-9, s->detect_err.v[0],
			s->detect_err.v[1], s->detect_err.v[2]);
	}
}

static inline void draw_sprite_crop(float width, float height, float x0, float y0, float x1, float y1);

static inline void scale_texture(struct face_tracker_filter *s, float scale)
{
	if (!s->texrender_scaled)
		s->texrender_scaled = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	const uint32_t cx = s->known_width / scale, cy = s->known_height / scale;
	gs_texrender_reset(s->texrender_scaled);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin(s->texrender_scaled, cx, cy)) {
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		gs_texture_t *tex = gs_texrender_get_texture(s->texrender);
		auto effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		if (tex && effect) {
			gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
			gs_effect_set_texture(image, tex);
			while (gs_effect_loop(effect, "Draw"))
				draw_sprite_crop(cx, cy, 0, 0, 1, 1);
		}
		gs_texrender_end(s->texrender_scaled);
	}
	gs_blend_state_pop();
}

static inline int stage_to_surface(struct face_tracker_filter *s, float scale)
{
	uint32_t width = s->known_width / scale;
	uint32_t height = s->known_height / scale;
	if (width <= 0 || height <= 0)
		return 1;

	gs_texture_t *tex = gs_texrender_get_texture(s->texrender_scaled);
	if (!tex)
		return 2;

	if (!s->stagesurface || width != gs_stagesurface_get_width(s->stagesurface) ||
	    height != gs_stagesurface_get_height(s->stagesurface)) {
		gs_stagesurface_destroy(s->stagesurface);
		s->stagesurface = gs_stagesurface_create(width, height, GS_BGRA);
	}

	gs_stage_texture(s->stagesurface, tex);

	return 0;
}

static inline std::shared_ptr<texture_object> surface_to_cvtex(struct face_tracker_filter *s, float scale)
{
	uint8_t *video_data = NULL;
	uint32_t video_linesize;
	if (!gs_stagesurface_map(s->stagesurface, &video_data, &video_linesize))
		return NULL;

	uint32_t width = gs_stagesurface_get_width(s->stagesurface);
	uint32_t height = gs_stagesurface_get_height(s->stagesurface);

	std::shared_ptr<texture_object> cvtex(new texture_object);
	cvtex->scale = scale;
	cvtex->tick = s->ftm->tick_cnt;

	struct obs_source_frame frame;
	memset(&frame, 0, sizeof(frame));
	frame.data[0] = video_data;
	frame.linesize[0] = video_linesize;
	frame.width = width;
	frame.height = height;
	frame.format = VIDEO_FORMAT_BGRA;
	cvtex->set_texture_obsframe(&frame, 1);

	gs_stagesurface_unmap(s->stagesurface);

	return cvtex;
}

static inline void draw_sprite_crop(float width, float height, float x0, float y0, float x1, float y1)
{
	gs_render_start(false);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(width, 0.0f);
	gs_vertex2f(0.0f, height);
	gs_vertex2f(width, height);
	struct vec2 tv;
	vec2_set(&tv, x0, y0);
	gs_texcoord2v(&tv, 0);
	vec2_set(&tv, x1, y0);
	gs_texcoord2v(&tv, 0);
	vec2_set(&tv, x0, y1);
	gs_texcoord2v(&tv, 0);
	vec2_set(&tv, x1, y1);
	gs_texcoord2v(&tv, 0);
	gs_render_stop(GS_TRISTRIP);
}

static inline void draw_frame_texture(struct face_tracker_filter *s, bool debug_notrack)
{
	uint32_t width = s->width_with_aspect;
	uint32_t height = s->height_with_aspect;
	const rectf_s &crop_cur = s->ftm->crop_cur;

	// TODO: linear_srgb, 27 only?

	if (width <= 0 || height <= 0)
		return;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_texture_t *tex = gs_texrender_get_texture(s->texrender);
	if (!tex)
		return;
	gs_effect_set_texture(image, tex);

	while (gs_effect_loop(effect, "Draw")) {
		if (debug_notrack)
			gs_draw_sprite(tex, 0, s->known_width, s->known_height);
		else
			draw_sprite_crop(width, height, crop_cur.x0 / s->known_width, crop_cur.y0 / s->known_height,
					 crop_cur.x1 / s->known_width, crop_cur.y1 / s->known_height);
	}
}

static inline void draw_frame_info(struct face_tracker_filter *s, bool debug_notrack, bool landmark_only = false)
{
	const rectf_s &crop_cur = s->ftm->crop_cur;
	bool draw_det = !landmark_only;
	bool draw_trk = !landmark_only;
	bool draw_lmk = true;
	bool draw_ref = !landmark_only;

	if (!debug_notrack) {
		uint32_t width = s->width_with_aspect;
		uint32_t height = s->height_with_aspect;
		const float scale =
			sqrtf((float)(width * height) / ((crop_cur.x1 - crop_cur.x0) * (crop_cur.y1 - crop_cur.y0)));

		gs_matrix_push();
		struct matrix4 tr;
		matrix4_identity(&tr);
		matrix4_translate3f(&tr, &tr, -(crop_cur.x0 + crop_cur.x1) * 0.5f, -(crop_cur.y0 + crop_cur.y1) * 0.5f,
				    0.0f);
		matrix4_scale3f(&tr, &tr, scale, scale, 1.0f);
		matrix4_translate3f(&tr, &tr, width / 2, height / 2, 0.0f);
		gs_matrix_mul(&tr);
	}

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	while (gs_effect_loop(effect, "Solid")) {
		if (draw_det) {
			gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), 0xFF0000FF);
			for (size_t i = 0; i < s->ftm->detect_rects.size(); i++)
				draw_rect_upsize(s->ftm->detect_rects[i], s->ftm->upsize_l, s->ftm->upsize_r,
						 s->ftm->upsize_t, s->ftm->upsize_b);
		}

		gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), 0xFF00FF00);
		for (size_t i = 0; i < s->ftm->tracker_rects.size(); i++) {
			const auto &tr = s->ftm->tracker_rects[i];
			if (draw_trk)
				draw_rect_upsize(tr.rect);
			if (draw_lmk && tr.landmark.size())
				draw_landmark(tr.landmark);
		}
		if (debug_notrack && draw_ref) {
			gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), 0xFFFFFF00); // amber
			const rectf_s &r = s->ftm->crop_cur;
			gs_render_start(false);
			gs_vertex2f(r.x0, r.y0);
			gs_vertex2f(r.x0, r.y1);
			gs_vertex2f(r.x0, r.y1);
			gs_vertex2f(r.x1, r.y1);
			gs_vertex2f(r.x1, r.y1);
			gs_vertex2f(r.x1, r.y0);
			gs_vertex2f(r.x1, r.y0);
			gs_vertex2f(r.x0, r.y0);
			const float srwhr2 = sqrtf((r.x1 - r.x0) * (r.y1 - r.y0)) * 0.5f;
			const float rcx = (r.x0 + r.x1) * 0.5f + (r.x1 - r.x0) * s->track_x;
			const float rcy = (r.y0 + r.y1) * 0.5f - (r.y1 - r.y0) * s->track_y;
			gs_vertex2f(rcx - srwhr2 * s->track_z, rcy);
			gs_vertex2f(rcx + srwhr2 * s->track_z, rcy);
			gs_vertex2f(rcx, rcy - srwhr2 * s->track_z);
			gs_vertex2f(rcx, rcy + srwhr2 * s->track_z);
			gs_render_stop(GS_LINES);

			if (s->ftm->landmark_detection_data) {
				gs_render_start(false);
				float r = srwhr2 * s->track_z;
				for (int i = 0; i <= 32; i++)
					gs_vertex2f(rcx + r * sinf(M_PI * i / 8), rcy + r * cosf(M_PI * i / 8));
				gs_render_stop(GS_LINESTRIP);
			}
		}
	}

	if (!debug_notrack)
		gs_matrix_pop();
}

static inline void calculate_relative_head_pose(const struct face_tracker_filter *s,
	const face_tracker_manager::tracker_rect_s &tr,
	float &yaw, float &pitch, float &roll)
{
	calculate_head_pose_3d(tr, yaw, pitch, roll);
	if (s->pose_3d_reference_set) {
		yaw -= s->pose_3d_reference_yaw;
		pitch -= s->pose_3d_reference_pitch;
		roll -= s->pose_3d_reference_roll;
		while (roll > (float)M_PI) roll -= (float)(2 * M_PI);
		while (roll < -(float)M_PI) roll += (float)(2 * M_PI);
	}
}

static inline void draw_face_prop(struct face_tracker_filter *s, bool debug_notrack)
{
	if (!s->prop_enabled || !s->prop_texture)
		return;

	// All-faces rendering is independent of the primary/selected face.
	// The selected-face tracking state is used for the camera crop, but it
	// must not gate rendering props on the other tracked faces.
	if (s->face_selection_mode == 4) {
		bool have_face = false;
		for (const auto &tr : s->ftm->tracker_rects) {
			if (tr.face_id > 0 && tr.rect.score > 0.0f) {
				have_face = true;
				break;
			}
		}
		if (!have_face)
			return;

		// Explicitly restore normal alpha blending for every prop instance.
		// OBS sources/filters can leave a different graphics blend state active.
		gs_blend_state_push();
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

		for (const auto &tr : s->ftm->tracker_rects) {
			if (tr.face_id <= 0 || tr.rect.score <= 0.0f)
				continue;

			float x, y, size, angle;
			calculate_overlay_target(tr, s->prop_anchor, x, y, size, angle);
			size = std::max(s->prop_min_size, std::min(s->prop_max_size, size));
			angle = std::max(-s->prop_max_rotation, std::min(s->prop_max_rotation, angle));

			float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
			calculate_relative_head_pose(s, tr, yaw, pitch, roll);

			x += s->prop_offset_x * size;
			y += s->prop_offset_y * size;
			float iw = (float)gs_texture_get_width(s->prop_texture);
			float ih = (float)gs_texture_get_height(s->prop_texture);
			if (iw <= 0 || ih <= 0)
				continue;
			float target_w = (s->prop_follow_size ? size : 120.0f) * s->prop_scale;
			float target_h = target_w * ih / iw;

			gs_matrix_push();
			if (!debug_notrack) {
				const rectf_s &crop = s->ftm->crop_cur;
				float scale = sqrtf((float)(s->width_with_aspect * s->height_with_aspect) /
					((crop.x1-crop.x0)*(crop.y1-crop.y0)));
				struct matrix4 trm; matrix4_identity(&trm);
				matrix4_translate3f(&trm,&trm,-(crop.x0+crop.x1)*0.5f,-(crop.y0+crop.y1)*0.5f,0.0f);
				matrix4_scale3f(&trm,&trm,scale,scale,1.0f);
				matrix4_translate3f(&trm,&trm,s->width_with_aspect/2.0f,s->height_with_aspect/2.0f,0.0f);
				gs_matrix_mul(&trm);
			}
			gs_matrix_translate3f(x, y, 0.0f);
			if (s->prop_3d_enabled) {
				float sx = 1.0f, sy = 1.0f;
				if (s->prop_3d_follow_yaw)
					sx = std::max(0.25f, fabsf(cosf(yaw * s->prop_3d_yaw_amount)));
				if (s->prop_3d_follow_pitch)
					sy = std::max(0.25f, fabsf(cosf(pitch * s->prop_3d_pitch_amount)));
				gs_matrix_scale3f(sx, sy, 1.0f);
				if (s->prop_3d_follow_roll)
					gs_matrix_rotaa4f(0.0f,0.0f,1.0f,roll * s->prop_3d_roll_amount);
			}
			if (s->prop_follow_rotation)
				gs_matrix_rotaa4f(0.0f,0.0f,1.0f,angle);
			if (s->prop_rotation != 0.0f)
				gs_matrix_rotaa4f(0.0f,0.0f,1.0f,s->prop_rotation * 0.01745329252f);
			gs_matrix_translate3f(-target_w*0.5f, -target_h*0.5f, 0.0f);

			gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
			gs_eparam_t *color = gs_effect_get_param_by_name(effect, "color");
			gs_effect_set_texture(image, s->prop_texture);
			if (color) {
				struct vec4 c; vec4_set(&c,1.0f,1.0f,1.0f,s->prop_render_opacity);
				gs_effect_set_vec4(color,&c);
			}
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(s->prop_texture, 0, (uint32_t)target_w, (uint32_t)target_h);
			gs_matrix_pop();
		}
		gs_blend_state_pop();
		return;
	}

	float x = s->prop_x + s->prop_offset_x * s->prop_size;
	float y = s->prop_y + s->prop_offset_y * s->prop_size;
	float iw = (float)gs_texture_get_width(s->prop_texture);
	float ih = (float)gs_texture_get_height(s->prop_texture);
	if (iw <= 0 || ih <= 0) return;
	float target_w = (s->prop_follow_size ? s->prop_size : 120.0f) * s->prop_scale;
	float target_h = target_w * ih / iw;

	gs_matrix_push();
	if (!debug_notrack) {
		const rectf_s &crop = s->ftm->crop_cur;
		float scale = sqrtf((float)(s->width_with_aspect * s->height_with_aspect) /
			((crop.x1-crop.x0)*(crop.y1-crop.y0)));
		struct matrix4 tr; matrix4_identity(&tr);
		matrix4_translate3f(&tr,&tr,-(crop.x0+crop.x1)*0.5f,-(crop.y0+crop.y1)*0.5f,0.0f);
		matrix4_scale3f(&tr,&tr,scale,scale,1.0f);
		matrix4_translate3f(&tr,&tr,s->width_with_aspect/2.0f,s->height_with_aspect/2.0f,0.0f);
		gs_matrix_mul(&tr);
	}
	gs_matrix_translate3f(x, y, 0.0f);
	if (s->prop_3d_enabled) {
		// Project the flat 2D texture instead of rotating the quad around X/Y.
		// OBS normally uses an orthographic projection here, so X/Y rotations
		// do not produce the expected visible pitch/yaw effect.
		float sx = 1.0f;
		float sy = 1.0f;
		if (s->prop_3d_follow_yaw)
			sx = std::max(0.25f, fabsf(cosf(s->prop_3d_yaw)));
		if (s->prop_3d_follow_pitch)
			sy = std::max(0.25f, fabsf(cosf(s->prop_3d_pitch)));
		gs_matrix_scale3f(sx, sy, 1.0f);
	}
	if (s->prop_3d_enabled && s->prop_3d_follow_roll)
		gs_matrix_rotaa4f(0.0f,0.0f,1.0f,s->prop_3d_roll);
	if (s->prop_follow_rotation) gs_matrix_rotaa4f(0.0f,0.0f,1.0f,s->prop_angle);
        if (s->prop_rotation != 0.0f) gs_matrix_rotaa4f(0.0f,0.0f,1.0f,s->prop_rotation * 0.01745329252f);
	gs_matrix_translate3f(-target_w*0.5f, -target_h*0.5f, 0.0f);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_eparam_t *color = gs_effect_get_param_by_name(effect, "color");
	gs_effect_set_texture(image, s->prop_texture);
	if (color) {
		struct vec4 c; vec4_set(&c,1.0f,1.0f,1.0f,s->prop_render_opacity);
		gs_effect_set_vec4(color,&c);
	}
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(s->prop_texture, 0, (uint32_t)target_w, (uint32_t)target_h);
	gs_matrix_pop();
}

static inline void draw_tracked_source(struct face_tracker_filter *s, bool debug_notrack)
{
        if (!s->tracked_source_enabled || !s->tracked_source_ref)
                return;
        if (!s->tracked_source_tracking && s->tracked_source_lost_behavior == 0)
                return;

        obs_source_t *source = obs_weak_source_get_source(s->tracked_source_ref);
        if (!source)
                return;

        const uint32_t sw = obs_source_get_width(source);
        const uint32_t sh = obs_source_get_height(source);
        if (sw == 0 || sh == 0) {
                obs_source_release(source);
                return;
        }

        if (!s->tracked_texrender)
                s->tracked_texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

        gs_texrender_reset(s->tracked_texrender);
        gs_blend_state_push();
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

        if (gs_texrender_begin(s->tracked_texrender, sw, sh)) {
                struct vec4 clear_color;
                vec4_zero(&clear_color);
                gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
                gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
                obs_source_video_render(source);
                gs_texrender_end(s->tracked_texrender);
        }

        gs_blend_state_pop();
        obs_source_release(source);

        gs_texture_t *tex = gs_texrender_get_texture(s->tracked_texrender);
        if (!tex)
                return;

        // All faces mode renders the selected OBS source once at every
        // currently tracked face. The source texture is shared between instances.
        if (s->face_selection_mode == 4) {
                bool have_face = false;
                for (const auto &tr : s->ftm->tracker_rects) {
                        if (tr.face_id > 0 && tr.rect.score > 0.0f) {
                                have_face = true;
                                break;
                        }
                }
                if (!have_face)
                        return;

                gs_blend_state_push();
                gs_enable_blending(true);
                gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

                for (const auto &tr : s->ftm->tracker_rects) {
                        if (tr.face_id <= 0 || tr.rect.score <= 0.0f)
                                continue;

                        float x, y, size, angle;
                        calculate_overlay_target(tr, 0, x, y, size, angle);
                        size = std::max(s->tracked_source_min_size,
                                std::min(s->tracked_source_max_size, size));
                        angle = std::max(-s->tracked_source_max_rotation,
                                std::min(s->tracked_source_max_rotation, angle));

                        float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
                        calculate_relative_head_pose(s, tr, yaw, pitch, roll);

                        x += s->tracked_source_offset_x * size;
                        y += s->tracked_source_offset_y * size;
                        float target_w = (s->tracked_source_follow_size ? size : 120.0f) *
                                s->tracked_source_scale;
                        float target_h = target_w * (float)sh / (float)sw;

                        gs_matrix_push();
                        if (!debug_notrack) {
                                const rectf_s &crop = s->ftm->crop_cur;
                                float scale = sqrtf((float)(s->width_with_aspect * s->height_with_aspect) /
                                        ((crop.x1-crop.x0)*(crop.y1-crop.y0)));
                                struct matrix4 trm;
                                matrix4_identity(&trm);
                                matrix4_translate3f(&trm, &trm,
                                        -(crop.x0+crop.x1)*0.5f, -(crop.y0+crop.y1)*0.5f, 0.0f);
                                matrix4_scale3f(&trm, &trm, scale, scale, 1.0f);
                                matrix4_translate3f(&trm, &trm,
                                        s->width_with_aspect/2.0f, s->height_with_aspect/2.0f, 0.0f);
                                gs_matrix_mul(&trm);
                        }

                        gs_matrix_translate3f(x, y, 0.0f);
                        if (s->tracked_source_3d_enabled) {
                                float sx = 1.0f, sy = 1.0f;
                                if (s->tracked_source_3d_follow_yaw)
                                        sx = std::max(0.25f,
                                                fabsf(cosf(yaw * s->tracked_source_3d_yaw_amount)));
                                if (s->tracked_source_3d_follow_pitch)
                                        sy = std::max(0.25f,
                                                fabsf(cosf(pitch * s->tracked_source_3d_pitch_amount)));
                                gs_matrix_scale3f(sx, sy, 1.0f);
                                if (s->tracked_source_3d_follow_roll)
                                        gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f,
                                                roll * s->tracked_source_3d_roll_amount);
                        }
                        if (s->tracked_source_follow_rotation)
                                gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, angle);
                        if (s->tracked_source_rotation != 0.0f)
                                gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f,
                                        s->tracked_source_rotation * 0.01745329252f);
                        gs_matrix_translate3f(-target_w * 0.5f, -target_h * 0.5f, 0.0f);

                        gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
                        gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
                        gs_eparam_t *color = gs_effect_get_param_by_name(effect, "color");
                        gs_effect_set_texture(image, tex);
                        if (color) {
                                struct vec4 c;
                                vec4_set(&c, 1.0f, 1.0f, 1.0f, s->tracked_source_opacity);
                                gs_effect_set_vec4(color, &c);
                        }
                        while (gs_effect_loop(effect, "Draw"))
                                gs_draw_quadf(tex, 0, target_w, target_h);
                        gs_matrix_pop();
                }
                gs_blend_state_pop();
                return;
        }

        float x = s->tracked_source_x + s->tracked_source_offset_x * s->tracked_source_size;
        float y = s->tracked_source_y + s->tracked_source_offset_y * s->tracked_source_size;
        float target_w = (s->tracked_source_follow_size ? s->tracked_source_size : 120.0f) * s->tracked_source_scale;
        float target_h = target_w * (float)sh / (float)sw;

        gs_matrix_push();
        if (!debug_notrack) {
                const rectf_s &crop = s->ftm->crop_cur;
                float scale = sqrtf((float)(s->width_with_aspect * s->height_with_aspect) /
                        ((crop.x1-crop.x0)*(crop.y1-crop.y0)));
                struct matrix4 tr;
                matrix4_identity(&tr);
                matrix4_translate3f(&tr, &tr, -(crop.x0+crop.x1)*0.5f, -(crop.y0+crop.y1)*0.5f, 0.0f);
                matrix4_scale3f(&tr, &tr, scale, scale, 1.0f);
                matrix4_translate3f(&tr, &tr, s->width_with_aspect/2.0f, s->height_with_aspect/2.0f, 0.0f);
                gs_matrix_mul(&tr);
        }

        gs_matrix_translate3f(x, y, 0.0f);
        if (s->tracked_source_3d_enabled) {
                float sx = 1.0f;
                float sy = 1.0f;
                if (s->tracked_source_3d_follow_yaw)
                        sx = std::max(0.25f, fabsf(cosf(s->tracked_source_3d_yaw)));
                if (s->tracked_source_3d_follow_pitch)
                        sy = std::max(0.25f, fabsf(cosf(s->tracked_source_3d_pitch)));
                gs_matrix_scale3f(sx, sy, 1.0f);
                if (s->tracked_source_3d_follow_roll)
                        gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, s->tracked_source_3d_roll);
        }
        if (s->tracked_source_follow_rotation)
        	gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, s->tracked_source_angle);
        if (s->tracked_source_rotation != 0.0f)
                gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, s->tracked_source_rotation * 0.01745329252f);
        gs_matrix_translate3f(-target_w * 0.5f, -target_h * 0.5f, 0.0f);

        gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
        gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
        gs_eparam_t *color = gs_effect_get_param_by_name(effect, "color");
        gs_effect_set_texture(image, tex);
        if (color) {
                struct vec4 c;
                vec4_set(&c, 1.0f, 1.0f, 1.0f, s->tracked_source_render_opacity);
                gs_effect_set_vec4(color, &c);
        }

        while (gs_effect_loop(effect, "Draw"))
                gs_draw_sprite(tex, 0, (uint32_t)target_w, (uint32_t)target_h);

        gs_matrix_pop();
}


static inline void draw_frame(struct face_tracker_filter *s)
{
	const bool debug_notrack = s->debug_notrack && (!s->is_active || s->debug_always_show);

	draw_frame_texture(s, debug_notrack);
	draw_face_prop(s, debug_notrack);
        draw_tracked_source(s, debug_notrack);

	if (s->debug_faces && (!s->is_active || s->debug_always_show))
		draw_frame_info(s, debug_notrack);
}

static void ftf_render(void *data, gs_effect_t *)
{
	auto *s = (struct face_tracker_filter *)data;
	if (!s->target_valid) {
		obs_source_skip_video_filter(s->context);
		return;
	}
	obs_source_t *target = obs_filter_get_target(s->context);
	obs_source_t *parent = obs_filter_get_parent(s->context);

	if (!target || !parent) {
		obs_source_skip_video_filter(s->context);
		return;
	}

	if (!s->rendered) {
		render_target(s, target, parent);
		if (is_running(s))
			s->ftm->post_render();
		s->rendered = true;
	}

	draw_frame(s);
}

static void fts_render(void *data, gs_effect_t *)
{
	auto *s = (struct face_tracker_filter *)data;
	if (!s->target_valid)
		return;

	obs_source_t *target = obs_weak_source_get_source(s->target_ref);

	if (!target)
		return;

	if (!s->rendered) {
		s->rendered = true;
		render_target(s, target, NULL);
		if (is_running(s))
			s->ftm->post_render();
	}

	draw_frame(s);

	obs_source_release(target);
}

static uint32_t ftf_width(void *data)
{
	auto *s = (struct face_tracker_filter *)data;
	const bool debug_notrack = s->debug_notrack && (!s->is_active || s->debug_always_show);
	return debug_notrack ? s->known_width : s->width_with_aspect;
}

static uint32_t ftf_height(void *data)
{
	auto *s = (struct face_tracker_filter *)data;
	const bool debug_notrack = s->debug_notrack && (!s->is_active || s->debug_always_show);
	return debug_notrack ? s->known_height : s->height_with_aspect;
}

static void cb_render_frame(void *data, calldata_t *cd)
{
	auto *s = (struct face_tracker_filter *)data;

	bool debug_notrack = false;
	calldata_get_bool(cd, "notrack", &debug_notrack);

	draw_frame_texture(s, debug_notrack);
}

static void cb_render_info(void *data, calldata_t *cd)
{
	auto *s = (struct face_tracker_filter *)data;

	bool debug_notrack = false;
	calldata_get_bool(cd, "notrack", &debug_notrack);

	bool landmark_only = false;
	calldata_get_bool(cd, "landmark_only", &landmark_only);

	draw_frame_info(s, debug_notrack, landmark_only);
}

static void cb_get_target_size(void *data, calldata_t *cd)
{
	auto *s = (struct face_tracker_filter *)data;
	calldata_set_int(cd, "width", (int)s->known_width);
	calldata_set_int(cd, "height", (int)s->known_height);
}

static void cb_get_state(void *data, calldata_t *cd)
{
	auto *s = (struct face_tracker_filter *)data;
	calldata_set_bool(cd, "paused", s->is_paused);
}

static void cb_set_state(void *data, calldata_t *cd)
{
	auto *s = (struct face_tracker_filter *)data;
	bool is_paused = s->is_paused;
	calldata_get_bool(cd, "paused", &is_paused);
	if (is_paused != s->is_paused) {
		s->is_paused = is_paused;
		emit_state_changed(s);
	}

	bool reset = false;
	calldata_get_bool(cd, "reset", &reset);
	if (reset)
		ftf_reset_tracking(NULL, NULL, s);
}

static void emit_face_event(struct face_tracker_filter *s, const char *signal, bool active, int face_id)
{
	struct calldata cd;
	uint8_t stack[128];
	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_ptr(&cd, "source", s->context);
	calldata_set_bool(&cd, "active", active);
	calldata_set_int(&cd, "face_id", face_id);
	signal_handler_t *sh = obs_source_get_signal_handler(s->context);
	signal_handler_signal(sh, signal, &cd);
}

static void emit_state_changed(struct face_tracker_filter *s)
{
	struct calldata cd;
	uint8_t stack[128];

	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_ptr(&cd, "source", s->context);
	cb_get_state(s, &cd);

	signal_handler_t *sh = obs_source_get_signal_handler(s->context);
	signal_handler_signal(sh, "state_changed", &cd);
}

extern "C" void register_face_tracker_filter(bool hide_filter, bool hide_source)
{
	struct obs_source_info info = {};
	info.id = "face_tracker_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	if (hide_filter)
		info.output_flags |= OBS_SOURCE_CAP_DISABLED;
	info.get_name = ftf_get_name;
	info.create = ftf_create;
	info.destroy = ftf_destroy;
	info.update = ftf_update;
	info.get_properties = ftf_properties;
	info.get_defaults = ftf_get_defaults;
	info.activate = ftf_activate;
	info.deactivate = ftf_deactivate;
	info.video_tick = ftf_tick;
	info.video_render = ftf_render;
	info.get_width = ftf_width;
	info.get_height = ftf_height;
	obs_register_source(&info);

	info.id = "face_tracker_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE;
	if (hide_source)
		info.output_flags |= OBS_SOURCE_CAP_DISABLED;
	info.create = fts_create;
	info.update = fts_update;
	info.get_properties = fts_properties;
	info.video_tick = fts_tick;
	info.video_render = fts_render;
	obs_register_source(&info);
}









