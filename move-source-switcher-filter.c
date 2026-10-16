#include "move-transition.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/dstr.h>
#include <util/darray.h>
#include <util/threading.h>

enum filter_mode {
	FILTER_MODE_INCLUDE = 0,
	FILTER_MODE_EXCLUDE = 1,
};

struct move_source_switcher_info {
	obs_source_t *source;
	obs_source_t *transition;
	obs_source_t *wrapper_scene_a;
	obs_source_t *wrapper_scene_b;

	obs_sceneitem_t *current_active_item;
	obs_sceneitem_t *outgoing_item;
	obs_sceneitem_t *incoming_item;

	bool transitioning;
	int internal_visibility_change;

	enum filter_mode mode;
	uint32_t duration;
	long long easing;
	long long easing_function;
	double curve;
	bool nested_scenes;
	bool name_part_match;
	bool name_number_match;
	bool name_last_word_match;
	uint32_t switch_percentage;

	signal_handler_t *connected_scene_signals;
	obs_source_t *parent_scene_source;

	DARRAY(char *) managed_source_names;
};

static const char *move_source_switcher_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("MoveSourceSwitcherFilter");
}

static void clean_wrapper_scenes(struct move_source_switcher_info *filter)
{
	if (filter->wrapper_scene_a) {
		obs_source_release(filter->wrapper_scene_a);
		filter->wrapper_scene_a = NULL;
	}
	if (filter->wrapper_scene_b) {
		obs_source_release(filter->wrapper_scene_b);
		filter->wrapper_scene_b = NULL;
	}
}

static void free_managed_sources(struct move_source_switcher_info *filter)
{
	for (size_t i = 0; i < filter->managed_source_names.num; i++) {
		bfree(filter->managed_source_names.array[i]);
	}
	da_free(filter->managed_source_names);
	da_init(filter->managed_source_names);
}

static bool is_item_managed(struct move_source_switcher_info *filter, obs_sceneitem_t *item)
{
	if (!item)
		return false;
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return false;
	const char *name = obs_source_get_name(source);
	if (!name || !strlen(name))
		return false;

	for (size_t i = 0; i < filter->managed_source_names.num; i++) {
		if (strcmp(filter->managed_source_names.array[i], name) == 0)
			return true;
	}
	return false;
}

static obs_source_t *create_wrapper_scene(obs_source_t *parent_scene, obs_sceneitem_t *item)
{
	if (!parent_scene)
		return NULL;

	uint32_t width = obs_source_get_width(parent_scene);
	uint32_t height = obs_source_get_height(parent_scene);
	if (!width)
		width = 1920;
	if (!height)
		height = 1080;

	obs_data_t *sd = obs_data_create();
	obs_data_set_bool(sd, "custom_size", true);
	obs_data_set_int(sd, "cx", width);
	obs_data_set_int(sd, "cy", height);
	obs_source_t *scene_source = obs_source_create_private("scene", "switcher_wrapper", sd);
	obs_source_load(scene_source);
	obs_data_release(sd);

	if (!scene_source)
		return NULL;

	if (!item)
		return scene_source;

	obs_scene_t *scene = obs_scene_from_source(scene_source);
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!scene || !source)
		return scene_source;

	obs_sceneitem_t *new_item = obs_scene_add(scene, source);
	if (new_item) {
		struct vec2 pos, scale, bounds;
		struct obs_sceneitem_crop crop;
		obs_sceneitem_get_pos(item, &pos);
		obs_sceneitem_get_scale(item, &scale);
		obs_sceneitem_get_bounds(item, &bounds);
		obs_sceneitem_get_crop(item, &crop);
		float rot = obs_sceneitem_get_rot(item);
		uint32_t align = obs_sceneitem_get_alignment(item);
		enum obs_bounds_type bounds_type = obs_sceneitem_get_bounds_type(item);
		uint32_t bounds_align = obs_sceneitem_get_bounds_alignment(item);

		obs_sceneitem_set_pos(new_item, &pos);
		obs_sceneitem_set_scale(new_item, &scale);
		obs_sceneitem_set_rot(new_item, rot);
		obs_sceneitem_set_crop(new_item, &crop);
		obs_sceneitem_set_alignment(new_item, align);
		obs_sceneitem_set_bounds_type(new_item, bounds_type);
		obs_sceneitem_set_bounds_alignment(new_item, bounds_align);
		obs_sceneitem_set_bounds(new_item, &bounds);
	}

	return scene_source;
}

static void start_switcher_transition(struct move_source_switcher_info *filter, obs_sceneitem_t *from_item,
				      obs_sceneitem_t *to_item)
{
	if (!to_item)
		return;

	obs_source_t *source_a = from_item ? obs_sceneitem_get_source(from_item) : NULL;
	obs_source_t *source_b = obs_sceneitem_get_source(to_item);
	if (!source_b)
		return;

	/* 3.3 Identical source collision */
	if (source_a && source_a == source_b) {
		filter->current_active_item = to_item;
		return;
	}

	if (!filter->transition) {
		obs_data_t *settings = obs_data_create();
		filter->transition = obs_source_create_private("move_transition", "internal_move_switcher", settings);
		obs_data_release(settings);
	}
	if (!filter->transition)
		return;

	obs_data_t *s = obs_source_get_settings(filter->transition);
	obs_data_set_int(s, S_EASING_MATCH, filter->easing);
	obs_data_set_int(s, S_EASING_FUNCTION_MATCH, filter->easing_function);
	obs_data_set_double(s, S_CURVE_MATCH, filter->curve);
	obs_data_set_bool(s, S_NESTED_SCENES, filter->nested_scenes);
	obs_data_set_bool(s, S_NAME_PART_MATCH, filter->name_part_match);
	obs_data_set_bool(s, S_NAME_NUMBER_MATCH, filter->name_number_match);
	obs_data_set_bool(s, S_NAME_LAST_WORD_MATCH, filter->name_last_word_match);
	obs_data_set_int(s, S_SWITCH_PERCENTAGE, (long long)filter->switch_percentage);
	obs_source_update(filter->transition, s);
	obs_data_release(s);

	/* 2.3 Rapid switching: stop previous transition cleanly */
	if (filter->transitioning) {
		obs_transition_force_stop(filter->transition);
		obs_transition_clear(filter->transition);
		clean_wrapper_scenes(filter);
		filter->transitioning = false;
	}

	obs_source_t *parent = filter->parent_scene_source ? filter->parent_scene_source : obs_filter_get_parent(filter->source);
	if (!parent)
		return;

	/* 3.1 "First Show" handling: if from_item is NULL and duration is 0, hard cut */
	if (!from_item && filter->duration == 0) {
		filter->current_active_item = to_item;
		filter->internal_visibility_change++;
		obs_sceneitem_set_visible(to_item, true);
		filter->internal_visibility_change--;
		return;
	}

	/* 1.3 Transform preservation via scene wrappers */
	filter->wrapper_scene_a = create_wrapper_scene(parent, from_item);
	filter->wrapper_scene_b = create_wrapper_scene(parent, to_item);

	obs_transition_set(filter->transition, filter->wrapper_scene_a);
	obs_transition_start(filter->transition, OBS_TRANSITION_MODE_AUTO, filter->duration, filter->wrapper_scene_b);

	filter->outgoing_item = from_item;
	filter->incoming_item = to_item;
	filter->transitioning = true;

	/* 3.2 UI Eye-Icon Flicker fix: Turn off previous item, keep new item checked */
	filter->internal_visibility_change++;
	if (from_item && from_item != to_item) {
		obs_sceneitem_set_visible(from_item, false);
	}
	obs_sceneitem_set_visible(to_item, true);
	filter->internal_visibility_change--;
}

static void on_item_visible(void *data, calldata_t *calldata)
{
	struct move_source_switcher_info *filter = data;
	/* 2.2 Reentrancy guard */
	if (filter->internal_visibility_change > 0)
		return;

	obs_sceneitem_t *item = (obs_sceneitem_t *)calldata_ptr(calldata, "item");
	bool visible = calldata_bool(calldata, "visible");
	if (!item)
		return;

	if (!is_item_managed(filter, item))
		return;

	if (visible) {
		if (item == filter->current_active_item && !filter->transitioning)
			return;

		obs_sceneitem_t *from_item = filter->current_active_item;
		/* 2.3 Rapid switching: transition from current incoming target */
		if (filter->transitioning && filter->incoming_item) {
			from_item = filter->incoming_item;
		}

		start_switcher_transition(filter, from_item, item);
	} else {
		if (item == filter->current_active_item && !filter->transitioning) {
			filter->current_active_item = NULL;
		}
	}
}

static void on_item_remove(void *data, calldata_t *calldata)
{
	struct move_source_switcher_info *filter = data;
	obs_sceneitem_t *item = (obs_sceneitem_t *)calldata_ptr(calldata, "item");
	if (!item)
		return;

	if (item == filter->current_active_item)
		filter->current_active_item = NULL;
	if (item == filter->outgoing_item)
		filter->outgoing_item = NULL;
	if (item == filter->incoming_item) {
		filter->incoming_item = NULL;
		/* 1.4 Lifetime hazard: if target is removed during transition, abort */
		if (filter->transitioning && filter->transition) {
			obs_transition_force_stop(filter->transition);
			obs_transition_clear(filter->transition);
			clean_wrapper_scenes(filter);
			filter->transitioning = false;
		}
	}
}

static void move_source_switcher_update(void *data, obs_data_t *settings);

static void on_source_rename(void *data, calldata_t *calldata)
{
	struct move_source_switcher_info *filter = data;
	const char *new_name = calldata_string(calldata, "new_name");
	const char *prev_name = calldata_string(calldata, "prev_name");
	if (!new_name || !prev_name || !filter->source)
		return;

	/* 4.2 Save and persist updated settings on source rename */
	obs_data_t *settings = obs_source_get_settings(filter->source);
	if (settings) {
		if (obs_data_has_user_value(settings, prev_name)) {
			bool val = obs_data_get_bool(settings, prev_name);
			obs_data_erase(settings, prev_name);
			obs_data_set_bool(settings, new_name, val);
			obs_source_update(filter->source, settings);
		}
		obs_data_release(settings);
	}
}

static void disconnect_parent_signals(struct move_source_switcher_info *filter);

static void on_parent_destroy(void *data, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	struct move_source_switcher_info *filter = data;
	if (filter->transitioning && filter->transition) {
		obs_transition_force_stop(filter->transition);
		obs_transition_clear(filter->transition);
		clean_wrapper_scenes(filter);
		filter->transitioning = false;
	}
	disconnect_parent_signals(filter);
	filter->current_active_item = NULL;
	filter->outgoing_item = NULL;
	filter->incoming_item = NULL;
}

static void disconnect_parent_signals(struct move_source_switcher_info *filter)
{
	if (filter->connected_scene_signals) {
		signal_handler_disconnect(filter->connected_scene_signals, "item_visible", on_item_visible, filter);
		signal_handler_disconnect(filter->connected_scene_signals, "item_remove", on_item_remove, filter);
		signal_handler_disconnect(filter->connected_scene_signals, "remove", on_parent_destroy, filter);
		signal_handler_disconnect(filter->connected_scene_signals, "destroy", on_parent_destroy, filter);
		filter->connected_scene_signals = NULL;
	}
	filter->parent_scene_source = NULL;
}

static void connect_parent_signals(struct move_source_switcher_info *filter)
{
	obs_source_t *parent = obs_filter_get_parent(filter->source);
	if (!parent)
		return;

	if (filter->parent_scene_source == parent && filter->connected_scene_signals)
		return;

	disconnect_parent_signals(filter);

	signal_handler_t *sh = obs_source_get_signal_handler(parent);
	if (sh) {
		signal_handler_connect(sh, "item_visible", on_item_visible, filter);
		signal_handler_connect(sh, "item_remove", on_item_remove, filter);
		signal_handler_connect(sh, "remove", on_parent_destroy, filter);
		signal_handler_connect(sh, "destroy", on_parent_destroy, filter);
		filter->connected_scene_signals = sh;
		filter->parent_scene_source = parent;
	}
}

static bool cache_managed_items(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct move_source_switcher_info *filter = param;
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;
	const char *name = obs_source_get_name(source);
	if (!name || !strlen(name))
		return true;

	obs_data_t *settings = obs_source_get_settings(filter->source);
	bool checked = obs_data_get_bool(settings, name);
	obs_data_release(settings);

	bool managed = (filter->mode == FILTER_MODE_INCLUDE) ? checked : !checked;
	if (managed) {
		char *name_copy = bstrdup(name);
		da_push_back(filter->managed_source_names, &name_copy);
	}
	return true;
}

static void *move_source_switcher_create(obs_data_t *settings, obs_source_t *source)
{
	struct move_source_switcher_info *filter = bzalloc(sizeof(struct move_source_switcher_info));
	filter->source = source;
	da_init(filter->managed_source_names);

	obs_data_t *trans_settings = obs_data_create();
	filter->transition = obs_source_create_private("move_transition", "internal_move_switcher", trans_settings);
	obs_data_release(trans_settings);

	signal_handler_connect(obs_get_signal_handler(), "source_rename", on_source_rename, filter);
	connect_parent_signals(filter);

	obs_source_update(source, settings);
	return filter;
}

static void move_source_switcher_destroy(void *data)
{
	struct move_source_switcher_info *filter = data;
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", on_source_rename, filter);
	disconnect_parent_signals(filter);

	if (filter->transition) {
		obs_transition_force_stop(filter->transition);
		obs_transition_clear(filter->transition);
		obs_source_release(filter->transition);
		filter->transition = NULL;
	}

	clean_wrapper_scenes(filter);
	free_managed_sources(filter);

	bfree(filter);
}

static void move_source_switcher_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct move_source_switcher_info *filter = data;

	/* 1.2 Fix: always render scene underneath so background/overlays are preserved */
	obs_source_skip_video_filter(filter->source);

	if (filter->transitioning && filter->transition) {
		obs_source_video_render(filter->transition);
	}
}

static void move_source_switcher_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct move_source_switcher_info *filter = data;

	/* 1.1 Fix: lazily connect parent signals if not yet connected */
	if (!filter->connected_scene_signals) {
		connect_parent_signals(filter);
		if (filter->connected_scene_signals && filter->managed_source_names.num == 0) {
			obs_data_t *s = obs_source_get_settings(filter->source);
			move_source_switcher_update(filter, s);
			obs_data_release(s);
		}
	}

	if (!filter->transitioning || !filter->transition)
		return;

	float t = obs_transition_get_time(filter->transition);
	bool active = obs_transition_get_active(filter->transition);
	if (t >= 1.0f || !active) {
		filter->transitioning = false;
		filter->current_active_item = filter->incoming_item;

		obs_transition_clear(filter->transition);
		clean_wrapper_scenes(filter);

		filter->internal_visibility_change++;
		if (filter->incoming_item)
			obs_sceneitem_set_visible(filter->incoming_item, true);
		if (filter->outgoing_item && filter->outgoing_item != filter->incoming_item)
			obs_sceneitem_set_visible(filter->outgoing_item, false);
		filter->internal_visibility_change--;

		filter->outgoing_item = NULL;
		filter->incoming_item = NULL;
	}
}

struct check_single_visibility_data {
	struct move_source_switcher_info *filter;
	bool found_active;
};

/* 3.4 Fix: non-destructive initial active item detection (no wiping visibility) */
static bool init_active_item(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct check_single_visibility_data *csv = param;
	struct move_source_switcher_info *filter = csv->filter;

	if (!is_item_managed(filter, item))
		return true;

	if (obs_sceneitem_visible(item)) {
		if (!csv->found_active) {
			csv->found_active = true;
			filter->current_active_item = item;
		}
	}
	return true;
}

static void move_source_switcher_update(void *data, obs_data_t *settings)
{
	struct move_source_switcher_info *filter = data;
	filter->mode = (enum filter_mode)obs_data_get_int(settings, "filter_mode");
	filter->duration = (uint32_t)obs_data_get_int(settings, S_DURATION);
	filter->easing = obs_data_get_int(settings, S_EASING_MATCH);
	filter->easing_function = obs_data_get_int(settings, S_EASING_FUNCTION_MATCH);
	filter->curve = obs_data_get_double(settings, S_CURVE_MATCH);
	filter->nested_scenes = obs_data_get_bool(settings, S_NESTED_SCENES);
	filter->name_part_match = obs_data_get_bool(settings, S_NAME_PART_MATCH);
	filter->name_number_match = obs_data_get_bool(settings, S_NAME_NUMBER_MATCH);
	filter->name_last_word_match = obs_data_get_bool(settings, S_NAME_LAST_WORD_MATCH);
	filter->switch_percentage = (uint32_t)obs_data_get_int(settings, S_SWITCH_PERCENTAGE);

	connect_parent_signals(filter);

	/* 4.3 Fix: cache managed source names in memory */
	free_managed_sources(filter);
	obs_source_t *parent = obs_filter_get_parent(filter->source);
	if (parent) {
		obs_scene_t *scene = obs_scene_from_source(parent);
		if (!scene)
			scene = obs_group_from_source(parent);
		if (scene) {
			obs_scene_enum_items(scene, cache_managed_items, filter);
			if (!filter->transitioning && !filter->current_active_item) {
				struct check_single_visibility_data csv = {filter, false};
				obs_scene_enum_items(scene, init_active_item, &csv);
			}
		}
	}
}

static bool add_item_checkbox(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	obs_properties_t *props = param;
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;
	const char *name = obs_source_get_name(source);
	if (!name || !strlen(name))
		return true;

	obs_properties_add_bool(props, name, name);
	return true;
}

static obs_properties_t *move_source_switcher_properties(void *data)
{
	struct move_source_switcher_info *filter = data;
	obs_properties_t *ppts = obs_properties_create();

	obs_source_t *parent = obs_filter_get_parent(filter->source);
	obs_scene_t *scene = obs_scene_from_source(parent);
	if (!scene)
		scene = obs_group_from_source(parent);

	if (!scene) {
		obs_property_t *w = obs_properties_add_text(ppts, "warning", obs_module_text("ScenesOnlyFilter"), OBS_TEXT_INFO);
		obs_property_text_set_info_type(w, OBS_TEXT_INFO_WARNING);
		return ppts;
	}

	obs_property_t *mode_list =
		obs_properties_add_list(ppts, "filter_mode", obs_module_text("FilterMode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode_list, obs_module_text("FilterMode.Include"), FILTER_MODE_INCLUDE);
	obs_property_list_add_int(mode_list, obs_module_text("FilterMode.Exclude"), FILTER_MODE_EXCLUDE);

	obs_properties_t *sources_group = obs_properties_create();
	obs_scene_enum_items(scene, add_item_checkbox, sources_group);
	obs_properties_add_group(ppts, "managed_sources", obs_module_text("ManagedSources"), OBS_GROUP_NORMAL, sources_group);

	obs_properties_t *trans_group = obs_properties_create();
	obs_property_t *p = obs_properties_add_int(trans_group, S_DURATION, obs_module_text("Duration"), 0, 10000, 50);
	obs_property_int_set_suffix(p, "ms");

	p = obs_properties_add_list(trans_group, S_EASING_MATCH, obs_module_text("Easing"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	prop_list_add_easings(p);

	p = obs_properties_add_list(trans_group, S_EASING_FUNCTION_MATCH, obs_module_text("EasingFunction"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	prop_list_add_easing_functions(p);

	obs_properties_add_float_slider(trans_group, S_CURVE_MATCH, obs_module_text("Curve"), -2.0, 2.0, 0.01);

	obs_properties_add_bool(trans_group, S_NESTED_SCENES, obs_module_text("NestedScenes"));

	obs_properties_add_bool(trans_group, S_NAME_PART_MATCH, obs_module_text("NamePartMatch"));
	obs_properties_add_bool(trans_group, S_NAME_NUMBER_MATCH, obs_module_text("NameNumberMatch"));
	obs_properties_add_bool(trans_group, S_NAME_LAST_WORD_MATCH, obs_module_text("NameLastWordMatch"));

	p = obs_properties_add_int_slider(trans_group, S_SWITCH_PERCENTAGE, obs_module_text("SwitchPoint"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");

	obs_properties_add_group(ppts, "transition_settings", obs_module_text("Transition"), OBS_GROUP_NORMAL, trans_group);

	return ppts;
}

static void move_source_switcher_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "filter_mode", FILTER_MODE_INCLUDE);
	obs_data_set_default_int(settings, S_DURATION, 500);
	obs_data_set_default_int(settings, S_EASING_MATCH, EASE_IN_OUT);
	obs_data_set_default_int(settings, S_EASING_FUNCTION_MATCH, EASING_CUBIC);
	obs_data_set_default_double(settings, S_CURVE_MATCH, 0.0);
	obs_data_set_default_bool(settings, S_NESTED_SCENES, true);
	obs_data_set_default_bool(settings, S_NAME_PART_MATCH, true);
	obs_data_set_default_bool(settings, S_NAME_NUMBER_MATCH, true);
	obs_data_set_default_bool(settings, S_NAME_LAST_WORD_MATCH, true);
	obs_data_set_default_int(settings, S_SWITCH_PERCENTAGE, 50);
}

struct obs_source_info move_source_switcher_filter = {
	.id = MOVE_SOURCE_SWITCHER_FILTER_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = move_source_switcher_get_name,
	.create = move_source_switcher_create,
	.destroy = move_source_switcher_destroy,
	.get_properties = move_source_switcher_properties,
	.get_defaults = move_source_switcher_defaults,
	.video_render = move_source_switcher_video_render,
	.video_tick = move_source_switcher_tick,
	.update = move_source_switcher_update,
	.load = move_source_switcher_update,
};
