#include "move-transition.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/dstr.h>
#include <util/threading.h>

struct move_source_switcher_info {
	obs_source_t *filter_source; //pointer to move source switcher filter itself
	obs_source_t *move_transition; //pointer to move transition
	obs_source_t *wrapper_scene_a; //pointers to temporary wrapper scene required by move transition, cleared after transisiton completed
	obs_source_t *wrapper_scene_b;

	obs_sceneitem_t *current_active_item; //pointer to current source (or nester scene) item the parent scene where this filter applied, this tracking required to decide sceneitem_a
	obs_sceneitem_t *sceneitem_a; //pointer to outgoing source (or nester scene) item  
	obs_sceneitem_t *sceneitem_b; //pointer to incoming source (or nester scene) item  

	bool transitioning; //flag to mark currently transition happening

	uint32_t duration; //current transition duration applied in the filter properties

	signal_handler_t *connected_scene_signals; //required to detect visibility changes and scene destruction

	char **excluded_sources; //NULL-terminated array of excluded source name strings
};

static const char *move_source_switcher_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("MoveSourceSwitcherFilter");
}

static void clean_wrapper_scenes(struct move_source_switcher_info *filter)
{
	if (filter->wrapper_scene_a)
	{
		obs_source_release(filter->wrapper_scene_a);
		filter->wrapper_scene_a = NULL;
	}
	if (filter->wrapper_scene_b)
	{
		obs_source_release(filter->wrapper_scene_b);
		filter->wrapper_scene_b = NULL;
	}
}

static inline bool is_source_excluded(struct move_source_switcher_info *filter, obs_source_t *source)
{
	if (!filter->excluded_sources || !source) return false;

	const char *name = obs_source_get_name(source);
	if (!name || !*name) return false;

	for (char **p = filter->excluded_sources; *p; p++)
	{
		if (astrcmpi(*p, name) == 0) return true;
	}
	return false;
}

struct find_sceneitem_index_data
{
	obs_sceneitem_t *target_item;
	int target_index;  //sceneitem index result
	int current_index;  //used for temporary storage while iterating
};

static bool find_sceneitem_index(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct find_sceneitem_index_data *data = param;
	if (item == data->target_item)
	{
		data->target_index = data->current_index;
		return false;
	}
	data->current_index++;
	return true;
}

struct adjust_sceneitem_visibility_data
{
	struct move_source_switcher_info *filter;
	int target_index;  //sceneitem index result
	int current_index;  //used for temporary storage while iterating
};
#define adjust_sceneitem_visivility_data adjust_sceneitem_visibility_data

static bool adjust_wrapper_scene_item_visibility(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct adjust_sceneitem_visibility_data *data = param;
	obs_source_t *source = obs_sceneitem_get_source(item);

	int idx = data->current_index++;

	if (is_source_excluded(data->filter, source)) return true;

	// Only target_item (by 1:1 index alignment) is visible in wrapper scene; other non-excluded items are hidden.
	// This uniquely isolates duplicate references with different transforms or filters.
	obs_sceneitem_set_visible(item, data->target_index >= 0 && idx == data->target_index);
	return true;
}

static obs_source_t *create_wrapper_scene(obs_source_t *parent_source, struct move_source_switcher_info *filter, obs_sceneitem_t *active_item)
{
	if (!parent_source) return NULL;

	obs_scene_t *parent_scene = obs_scene_from_source(parent_source);
	if (!parent_scene) parent_scene = obs_group_from_source(parent_source);
	if (!parent_scene) return NULL;

	obs_scene_t *dup_scene = obs_scene_duplicate(parent_scene, "switcher_wrapper", OBS_SCENE_DUP_PRIVATE_REFS);
	if (!dup_scene) return NULL;

	struct find_sceneitem_index_data find_data =
	{
		.target_item = active_item,
		.target_index = -1,
		.current_index = 0,
	};
	if (active_item)
	{
		obs_scene_enum_items(parent_scene, find_sceneitem_index, &find_data);
	}

	struct adjust_sceneitem_visibility_data data =
	{
		.filter = filter,
		.target_index = find_data.target_index,
		.current_index = 0,
	};
	obs_scene_enum_items(dup_scene, adjust_wrapper_scene_item_visibility, &data);

	return obs_scene_get_source(dup_scene);
}

static void start_switcher_transition(struct move_source_switcher_info *filter, obs_sceneitem_t *from_item, obs_sceneitem_t *to_item)
{
	if (!to_item) return;

	obs_source_t *source_b = obs_sceneitem_get_source(to_item);
	if (!source_b) return;

	//Ignore if switching to the exact same item already active
	if (from_item && from_item == to_item) return;
	
	if (!filter->move_transition)
	{
		obs_data_t *settings = obs_source_get_settings(filter->filter_source);
		filter->move_transition = obs_source_create_private("move_transition", "internal_move_switcher", settings);
		obs_data_release(settings);
	}
	if (!filter->move_transition) return;

	//if transition already happening, stop previous transition cleanly 
	if (filter->transitioning)
	{
		obs_transition_force_stop(filter->move_transition);
		obs_transition_clear(filter->move_transition);
		clean_wrapper_scenes(filter);

		//Immediately turn off the superseded outgoing source so it doesn't linger 
		if (filter->sceneitem_a && filter->sceneitem_a != to_item && filter->sceneitem_a != from_item) obs_sceneitem_set_visible(filter->sceneitem_a, false);

		filter->transitioning = false;
	}

	obs_source_t *parent = obs_filter_get_parent(filter->filter_source);
	if (!parent) return;

	//First Show handling: if from_item is NULL and duration is 0, hard cut 
	if (!from_item && filter->duration == 0)
	{
		filter->current_active_item = to_item;
		return;
	}

	filter->sceneitem_a = from_item;
	filter->sceneitem_b = to_item;

	//Create wrapper scenes duplicating the full parent scene: Wrapper A has from_item visible; Wrapper B has to_item visible
	filter->wrapper_scene_a = create_wrapper_scene(parent, filter, from_item);
	filter->wrapper_scene_b = create_wrapper_scene(parent, filter, to_item);

	if (!filter->wrapper_scene_a || !filter->wrapper_scene_b)
	{
		clean_wrapper_scenes(filter);
		filter->sceneitem_a = NULL;
		filter->sceneitem_b = NULL;
		return;
	}

	obs_transition_set(filter->move_transition, filter->wrapper_scene_a);
	obs_transition_start(filter->move_transition, OBS_TRANSITION_MODE_AUTO, filter->duration, filter->wrapper_scene_b);

	filter->transitioning = true;

	//Note: In OBS UI, both eye icons stay visible throughout the transition, it will be turned off when the transition completes in video_tick.
}

static void on_item_visible(void *data, calldata_t *calldata)
{
	struct move_source_switcher_info *filter = data;
	if (!obs_source_enabled(filter->filter_source)) return;

	obs_sceneitem_t *item = (obs_sceneitem_t *)calldata_ptr(calldata, "item");
	bool visible = calldata_bool(calldata, "visible");
	if (!item) return;

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (is_source_excluded(filter, source)) return;

	if (visible)
	{
		if (item == filter->current_active_item && !filter->transitioning) return;

		//user transitioned to different source in mid of transition
		obs_sceneitem_t *from_item = filter->current_active_item;
		if (filter->transitioning && filter->sceneitem_b) from_item = filter->sceneitem_b;
		
		start_switcher_transition(filter, from_item, item);
	}
	else
	{
		if (item == filter->sceneitem_a) return;
		if (item == filter->current_active_item && !filter->transitioning) filter->current_active_item = NULL;		
	}
}

static void on_item_remove(void *data, calldata_t *calldata)
{
	struct move_source_switcher_info *filter = data;
	obs_sceneitem_t *item = (obs_sceneitem_t *)calldata_ptr(calldata, "item");
	if (!item) return;

	if (item == filter->current_active_item) filter->current_active_item = NULL;
	if (item == filter->sceneitem_a) filter->sceneitem_a = NULL;
	if (item == filter->sceneitem_b)
	{
		filter->sceneitem_b = NULL;
		if (filter->transitioning && filter->move_transition)
		{
			obs_transition_force_stop(filter->move_transition);
			obs_transition_clear(filter->move_transition);
			clean_wrapper_scenes(filter);
			filter->transitioning = false;
		}
	}
}

static void disconnect_parent_signals(struct move_source_switcher_info *filter);

static void on_parent_destroy(void *data, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	struct move_source_switcher_info *filter = data;
	if (filter->transitioning && filter->move_transition)
	{
		obs_transition_force_stop(filter->move_transition);
		obs_transition_clear(filter->move_transition);
		clean_wrapper_scenes(filter);
		filter->transitioning = false;
	}
	disconnect_parent_signals(filter);
	filter->current_active_item = NULL;
	filter->sceneitem_a = NULL;
	filter->sceneitem_b = NULL;
}

static void disconnect_parent_signals(struct move_source_switcher_info *filter)
{
	if (filter->connected_scene_signals)
	{
		signal_handler_disconnect(filter->connected_scene_signals, "item_visible", on_item_visible, filter);
		signal_handler_disconnect(filter->connected_scene_signals, "item_remove", on_item_remove, filter);
		signal_handler_disconnect(filter->connected_scene_signals, "remove", on_parent_destroy, filter);
		signal_handler_disconnect(filter->connected_scene_signals, "destroy", on_parent_destroy, filter);
		filter->connected_scene_signals = NULL;
	}
}

static void connect_parent_signals(struct move_source_switcher_info *filter)
{
	obs_source_t *parent = obs_filter_get_parent(filter->filter_source);
	if (!parent)
	{
		disconnect_parent_signals(filter);
		return;
	}

	signal_handler_t *sh = obs_source_get_signal_handler(parent);
	if (sh == filter->connected_scene_signals) return;

	disconnect_parent_signals(filter);

	if (sh)
	{
		signal_handler_connect(sh, "item_visible", on_item_visible, filter);
		signal_handler_connect(sh, "item_remove", on_item_remove, filter);
		signal_handler_connect(sh, "remove", on_parent_destroy, filter);
		signal_handler_connect(sh, "destroy", on_parent_destroy, filter);
		filter->connected_scene_signals = sh;
	}
}

static bool init_active_item(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct move_source_switcher_info *filter = param;

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (is_source_excluded(filter, source)) return true;

	if (obs_sceneitem_visible(item))
	{
		filter->current_active_item = item;
		return false;
	}
	return true;
}

static void on_filter_enable(void *data, calldata_t *calldata)
{
	struct move_source_switcher_info *filter = data;
	bool enabled = calldata_bool(calldata, "enabled");
	if (!enabled)
	{
		if (filter->transitioning)
		{
			if (filter->move_transition)
			{
				obs_transition_force_stop(filter->move_transition);
				obs_transition_clear(filter->move_transition);
			}
			clean_wrapper_scenes(filter);
			filter->transitioning = false;
			filter->sceneitem_a = NULL;
			filter->sceneitem_b = NULL;
		}
	}
	else
	{
		filter->current_active_item = NULL;
		obs_source_t *parent = obs_filter_get_parent(filter->filter_source);
		if (parent)
		{
			obs_scene_t *scene = obs_scene_from_source(parent);
			if (!scene) scene = obs_group_from_source(parent);
			if (scene)
			{
				obs_scene_enum_items(scene, init_active_item, filter);
			}
		}
	}
}

static void *move_source_switcher_create(obs_data_t *settings, obs_source_t *source)
{
	struct move_source_switcher_info *filter = bzalloc(sizeof(struct move_source_switcher_info));
	filter->filter_source = source;

	filter->move_transition = obs_source_create_private("move_transition", "internal_move_switcher", settings);

	signal_handler_connect(obs_source_get_signal_handler(source), "enable", on_filter_enable, filter);
	connect_parent_signals(filter);

	obs_source_update(source, settings);
	return filter;
}

static void move_source_switcher_destroy(void *data)
{
	struct move_source_switcher_info *filter = data;
	if (filter->filter_source)
	{
		signal_handler_disconnect(obs_source_get_signal_handler(filter->filter_source), "enable", on_filter_enable, filter);
	}
	disconnect_parent_signals(filter);

	if (filter->move_transition)
	{
		obs_transition_force_stop(filter->move_transition);
		obs_transition_clear(filter->move_transition);
		obs_source_release(filter->move_transition);
		filter->move_transition = NULL;
	}

	clean_wrapper_scenes(filter);

	strlist_free(filter->excluded_sources);
	filter->excluded_sources = NULL;

	bfree(filter);
}

static void move_source_switcher_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct move_source_switcher_info *filter = data;

	if (obs_source_enabled(filter->filter_source) && filter->transitioning && filter->move_transition)
	{
		//TRANSITION PASS: The move transition renders the full scene composite (wrapper_scene_a -> wrapper_scene_b) instead of normal rendering.
		obs_source_video_render(filter->move_transition);
	}
	else
	{
		//NORMAL PASS: No transition active. Parent scene renders normally. 
		obs_source_skip_video_filter(filter->filter_source);
	}
}

static void move_source_switcher_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct move_source_switcher_info *filter = data;

	if (!filter->connected_scene_signals) connect_parent_signals(filter);
	
	// if filter not active, stop transition and clean-up
	if (!obs_source_enabled(filter->filter_source))
	{
		if (filter->transitioning)
		{
			if (filter->move_transition)
			{
				obs_transition_force_stop(filter->move_transition);
				obs_transition_clear(filter->move_transition);
			}
			clean_wrapper_scenes(filter);
			filter->transitioning = false;
			filter->sceneitem_a = NULL;
			filter->sceneitem_b = NULL;
		}
		return;
	}

	// if not transitioning return early
	if (!filter->transitioning || !filter->move_transition) return;

	// if transition time completed, stop transiton and clean-up
	float t = obs_transition_get_time(filter->move_transition);
	if (t >= 1.0f)
	{
		filter->current_active_item = filter->sceneitem_b;
		
		obs_transition_clear(filter->move_transition);
		clean_wrapper_scenes(filter);
		filter->transitioning = false;

		//Transition finished: turn off sceneitem_a's eye, keep sceneitem_b on
		if (filter->sceneitem_a && filter->sceneitem_a != filter->sceneitem_b && obs_sceneitem_visible(filter->sceneitem_a)) obs_sceneitem_set_visible(filter->sceneitem_a, false);

		filter->sceneitem_a = NULL;
		filter->sceneitem_b = NULL;
	}
}

static void move_source_switcher_update(void *data, obs_data_t *settings)
{
	struct move_source_switcher_info *filter = data;
	filter->duration = (uint32_t)obs_data_get_int(settings, S_DURATION);

	strlist_free(filter->excluded_sources);
	filter->excluded_sources = NULL;

	const char *excluded_str = obs_data_get_string(settings, "excluded_sources");
	if (excluded_str && *excluded_str)
	{
		char *copy = bstrdup(excluded_str);
		for (char *c = copy; *c; c++)
		{
			if (*c == '\n' || *c == '\r') *c = ',';
		}
		filter->excluded_sources = strlist_split(copy, ',', false);
		bfree(copy);

		for (char **p = filter->excluded_sources; p && *p; p++)
		{
			char *str = *p;
			while (*str == ' ' || *str == '\t') str++;
			if (str != *p) memmove(*p, str, strlen(str) + 1);
			size_t len = strlen(*p);
			while (len > 0 && ((*p)[len - 1] == ' ' || (*p)[len - 1] == '\t')) (*p)[--len] = '\0';
		}
	}

	if (filter->move_transition) obs_source_update(filter->move_transition, settings);
	else filter->move_transition = obs_source_create_private("move_transition", "internal_move_switcher", settings);
	
	connect_parent_signals(filter);

	obs_source_t *parent = obs_filter_get_parent(filter->filter_source);
	if (parent)
	{
		obs_scene_t *scene = obs_scene_from_source(parent);
		if (!scene) scene = obs_group_from_source(parent);
		if (scene)
		{
			if (!filter->transitioning && !filter->current_active_item)
			{
				obs_scene_enum_items(scene, init_active_item, filter);
			}
		}
	}
}

static obs_properties_t *move_source_switcher_properties(void *data)
{
	struct move_source_switcher_info *filter = data;
	obs_properties_t *ppts = obs_properties_create();

	obs_source_t *parent = obs_filter_get_parent(filter->filter_source);
	obs_scene_t *scene = obs_scene_from_source(parent);
	if (!scene) scene = obs_group_from_source(parent);

	if (!scene)
	{
		obs_property_t *w = obs_properties_add_text(ppts, "warning", obs_module_text("ScenesOnlyFilter"), OBS_TEXT_INFO);
		obs_property_text_set_info_type(w, OBS_TEXT_INFO_WARNING);
		return ppts;
	}

	obs_properties_add_text(ppts, "excluded_sources", obs_module_text("ExcludedSources"), OBS_TEXT_MULTILINE);

	obs_property_t *p = obs_properties_add_int(ppts, S_DURATION, obs_module_text("Duration"), 0, 10000, 50);
	obs_property_int_set_suffix(p, "ms");

	// General
	obs_properties_t *group = obs_properties_create();
	obs_properties_add_bool(group, S_NESTED_SCENES, obs_module_text("NestedScenes"));
	obs_properties_add_bool(group, S_CACHE_TRANSITIONS, obs_module_text("CacheTransitions"));
	p = obs_properties_add_int_slider(group, S_SWITCH_PERCENTAGE, obs_module_text("SwitchPoint"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_group(ppts, S_MOVE_ALL, obs_module_text("MoveAll"), OBS_GROUP_NORMAL, group);

	// Match if the source name...
	group = obs_properties_create();
	obs_properties_add_bool(group, S_NAME_PART_MATCH, obs_module_text("NamePartMatch"));
	obs_properties_add_bool(group, S_NAME_NUMBER_MATCH, obs_module_text("NameNumberMatch"));
	obs_properties_add_bool(group, S_NAME_LAST_WORD_MATCH, obs_module_text("NameLastWordMatch"));
	obs_properties_add_group(ppts, S_MATCH, obs_module_text("MatchName"), OBS_GROUP_NORMAL, group);

	// Matched items
	group = obs_properties_create();
	p = obs_properties_add_list(group, S_EASING_MATCH, obs_module_text("Easing"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	prop_list_add_easings(p);
	p = obs_properties_add_list(group, S_EASING_FUNCTION_MATCH, obs_module_text("EasingFunction"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	prop_list_add_easing_functions(p);
	p = obs_properties_add_list(group, S_TRANSITION_MATCH, obs_module_text("Transition"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	prop_list_add_transitions(p);
	p = obs_properties_add_list(group, S_TRANSITION_SCALE, obs_module_text("TransitionScaleType"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	prop_list_add_scales(p);
	obs_properties_add_float_slider(group, S_CURVE_MATCH, obs_module_text("Curve"), -2.0, 2.0, 0.01);
	obs_properties_add_group(ppts, S_MOVE_MATCH, obs_module_text("MoveMatch"), OBS_GROUP_NORMAL, group);

	// Appearing items
	group = obs_properties_create();
	p = obs_properties_add_list(group, S_EASING_IN, obs_module_text("Easing"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	prop_list_add_easings(p);
	p = obs_properties_add_list(group, S_EASING_FUNCTION_IN, obs_module_text("EasingFunction"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	prop_list_add_easing_functions(p);
	p = obs_properties_add_float_slider(group, S_ZOOM_IN, obs_module_text("Zoom"), 0.0, 200.0, 1.0);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_list(group, S_POSITION_IN, obs_module_text("Position"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	prop_list_add_positions(p);
	p = obs_properties_add_list(group, S_TRANSITION_IN, obs_module_text("Transition"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	prop_list_add_transitions(p);
	obs_properties_add_float_slider(group, S_CURVE_IN, obs_module_text("Curve"), -2.0, 2.0, 0.01);
	obs_properties_add_group(ppts, S_MOVE_IN, obs_module_text("MoveIn"), OBS_GROUP_NORMAL, group);

	// Disappearing items
	group = obs_properties_create();
	p = obs_properties_add_list(group, S_EASING_OUT, obs_module_text("Easing"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	prop_list_add_easings(p);
	p = obs_properties_add_list(group, S_EASING_FUNCTION_OUT, obs_module_text("EasingFunction"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	prop_list_add_easing_functions(p);
	p = obs_properties_add_float_slider(group, S_ZOOM_OUT, obs_module_text("Zoom"), 0.0, 200.0, 1.0);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_list(group, S_POSITION_OUT, obs_module_text("Position"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	prop_list_add_positions(p);
	p = obs_properties_add_list(group, S_TRANSITION_OUT, obs_module_text("Transition"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	prop_list_add_transitions(p);
	obs_properties_add_float_slider(group, S_CURVE_OUT, obs_module_text("Curve"), -2.0, 2.0, 0.01);
	obs_properties_add_group(ppts, S_MOVE_OUT, obs_module_text("MoveOut"), OBS_GROUP_NORMAL, group);

	return ppts;
}

static void move_source_switcher_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "excluded_sources", "");
	obs_data_set_default_int(settings, S_DURATION, 600);
	move_defaults(settings);
	obs_data_set_default_bool(settings, S_NAME_PART_MATCH, true);
	obs_data_set_default_bool(settings, S_NAME_NUMBER_MATCH, true);
	obs_data_set_default_bool(settings, S_NAME_LAST_WORD_MATCH, true);
	obs_data_set_default_bool(settings, S_CACHE_TRANSITIONS, false);
}

struct obs_source_info move_source_switcher_filter =
{
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
