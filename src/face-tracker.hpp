#pragma once

#include <vector>
#include <deque>
#include "helper.hpp"

struct face_tracker_filter
{
	obs_source_t *context;
	gs_texrender_t *texrender;
        gs_texrender_t *tracked_texrender;
	gs_texrender_t *texrender_scaled;
	gs_stagesurf_t *stagesurface;
	uint32_t known_width;
	uint32_t known_height;
	uint32_t width_with_aspect;
	uint32_t height_with_aspect;
	bool target_valid;
	bool rendered;
	bool is_active;

	f3 detect_err;
	f3 range_min, range_max, range_min_out;

	class ft_manager_for_ftf *ftm;

	float track_z, track_x, track_y;
	float scale_max;

	// Face Prop overlay
	bool prop_enabled;
	char *prop_path;
	gs_texture_t *prop_texture;
	float prop_scale, prop_offset_x, prop_offset_y, prop_opacity, prop_rotation;
	float prop_pos_smoothing, prop_scale_smoothing, prop_rotation_smoothing;
	float prop_min_size, prop_max_size, prop_max_rotation;
	int prop_anchor, prop_lost_behavior;
	float prop_lost_fade_time, prop_lost_elapsed, prop_render_opacity;
	bool prop_follow_size, prop_follow_rotation, prop_hide_lost;
	float prop_x, prop_y, prop_size, prop_angle;
	bool prop_tracking;
	// 3D-style head pose (yaw/pitch/roll) derived from facial landmarks
	bool prop_3d_enabled, prop_3d_follow_yaw, prop_3d_follow_pitch, prop_3d_follow_roll;
	float prop_3d_yaw, prop_3d_pitch, prop_3d_roll;
	float prop_3d_yaw_smoothing, prop_3d_pitch_smoothing, prop_3d_roll_smoothing;
	float prop_3d_yaw_amount, prop_3d_pitch_amount, prop_3d_roll_amount;

        // Tracked OBS source / scene
        bool tracked_source_enabled;
        char *tracked_source_name;
        obs_weak_source_t *tracked_source_ref;
        float tracked_source_scale, tracked_source_offset_x, tracked_source_offset_y, tracked_source_opacity, tracked_source_rotation;
        float tracked_source_pos_smoothing, tracked_source_scale_smoothing, tracked_source_rotation_smoothing;
        float tracked_source_min_size, tracked_source_max_size, tracked_source_max_rotation;
        int tracked_source_lost_behavior;
        float tracked_source_lost_fade_time, tracked_source_lost_elapsed, tracked_source_render_opacity;
        bool tracked_source_follow_size, tracked_source_follow_rotation;
        float tracked_source_x, tracked_source_y, tracked_source_size, tracked_source_angle;
        bool tracked_source_tracking;
        // 3D-style head pose (yaw/pitch/roll) derived from facial landmarks
        bool tracked_source_3d_enabled, tracked_source_3d_follow_yaw, tracked_source_3d_follow_pitch, tracked_source_3d_follow_roll;
        float tracked_source_3d_yaw, tracked_source_3d_pitch, tracked_source_3d_roll;
        float tracked_source_3d_yaw_smoothing, tracked_source_3d_pitch_smoothing, tracked_source_3d_roll_smoothing;
        float tracked_source_3d_yaw_amount, tracked_source_3d_pitch_amount, tracked_source_3d_roll_amount;

        // Face events
        bool face_size_trigger_enabled;
        float face_size_trigger_min, face_size_trigger_max;
        bool face_size_triggered;

	f3 kp;
	float ki;
	f3 klpf;
	f3 tlpf;
	f3 e_deadband, e_nonlinear; // deadband and nonlinear amount for error input
	f3 filter_int_out;
	f3 filter_int;
	f3 filter_lpf;
	f3 u_last;
	int aspect_x, aspect_y;

	// face tracker source
	char *target_name;
	obs_weak_source_t *target_ref;

	bool inactive_reset = false;
	bool debug_faces;
	bool debug_notrack;
	bool debug_always_show;
	FILE *debug_data_tracker;
	FILE *debug_data_error;
	FILE *debug_data_control;
	char *debug_data_tracker_last;
	char *debug_data_error_last;
	char *debug_data_control_last;

	bool is_paused;
	obs_hotkey_pair_id hotkey_pause;
	obs_hotkey_id hotkey_reset;
};

