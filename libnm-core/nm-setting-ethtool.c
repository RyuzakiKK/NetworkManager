/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */

/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * Copyright 2018 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nm-setting-ethtool.h"

#include "nm-setting-private.h"
#include "nm-ethtool-utils.h"

/*****************************************************************************/

/**
 * SECTION:nm-setting-ethtool
 * @short_description: Describes connection properties for ethtool related options
 *
 * The #NMSettingEthtool object is a #NMSetting subclass that describes properties
 * to control network driver and hardware settings.
 **/

/*****************************************************************************/

/**
 * nm_ethtool_optname_is_offload_feature:
 * @optname: the option name to check
 *
 * Checks whether @optname is a valid option name for an offload feature.
 *
 * %Returns: %TRUE, if @optname is valid
 *
 * Since: 1.14
 */
gboolean
nm_ethtool_optname_is_offload_feature (const char *optname)
{
	return optname && nm_ethtool_id_is_offload_feature (nm_ethtool_id_get_by_name (optname));
}

/*****************************************************************************/

NM_GOBJECT_PROPERTIES_DEFINE (NMSettingEthtool,
	PROP_OPTS,
);

typedef struct {
} NMSettingEthtoolPrivate;

/**
 * NMSettingEthtool:
 *
 * Ethtool Ethernet Settings
 *
 * Since: 1.14
 */
struct _NMSettingEthtool {
	NMSetting parent;
	NMSettingEthtoolPrivate _priv;
};

struct _NMSettingEthtoolClass {
	NMSettingClass parent;
};

G_DEFINE_TYPE (NMSettingEthtool, nm_setting_ethtool, NM_TYPE_SETTING)

#define NM_SETTING_ETHTOOL_GET_PRIVATE(self) _NM_GET_PRIVATE (self, NMSettingEthtool, NM_IS_SETTING_ETHTOOL, NMSetting)

/*****************************************************************************/

static void
_notify_attributes (NMSettingEthtool *self)
{
	_nm_setting_gendata_notify (NM_SETTING (self), TRUE);
	_notify (self, PROP_OPTS);
}

/*****************************************************************************/

/**
 * nm_setting_ethtool_get_offload_feature:
 * @setting: the #NMSettingEthtool
 * @optname: option name of the offload feature to get
 *
 * Gets and offload feature setting. Returns %NM_TERNARY_DEFAULT if the
 * feature is not set.
 *
 * Returns: a #NMTernary value indicating whether the offload feature
 *   is enabled, disabled, or left untouched.
 *
 * Since: 1.14
 */
NMTernary
nm_setting_ethtool_get_offload_feature (NMSettingEthtool *setting,
                                        const char *optname)
{
	GVariant *v;

	g_return_val_if_fail (NM_IS_SETTING_ETHTOOL (setting), NM_TERNARY_DEFAULT);
	g_return_val_if_fail (optname && nm_ethtool_optname_is_offload_feature (optname), NM_TERNARY_DEFAULT);

	v = nm_setting_gendata_get (NM_SETTING (setting), optname);
	if (   v
	    && g_variant_is_of_type (v, G_VARIANT_TYPE_BOOLEAN)) {
		return g_variant_get_boolean (v)
		       ? NM_TERNARY_TRUE
		       : NM_TERNARY_FALSE;
	}
	return NM_TERNARY_DEFAULT;
}

/**
 * nm_setting_ethtool_set_offload_feature:
 * @setting: the #NMSettingEthtool
 * @optname: option name of the offload feature to get
 * @value: the new value to set. The special value %NM_TERNARY_DEFAULT
 *   means to clear the offload feature setting.
 *
 * Sets and offload feature setting.
 *
 * Since: 1.14
 */
void
nm_setting_ethtool_set_offload_feature (NMSettingEthtool *setting,
                                        const char *optname,
                                        NMTernary value)
{
	GHashTable *hash;
	GVariant *v;

	g_return_if_fail (NM_IS_SETTING_ETHTOOL (setting));
	g_return_if_fail (optname && nm_ethtool_optname_is_offload_feature (optname));
	g_return_if_fail (NM_IN_SET (value, NM_TERNARY_DEFAULT,
	                                    NM_TERNARY_FALSE,
	                                    NM_TERNARY_TRUE));

	hash = _nm_setting_gendata_hash (NM_SETTING (setting),
	                                 value != NM_TERNARY_DEFAULT);

	if (value == NM_TERNARY_DEFAULT) {
		if (hash) {
			if (g_hash_table_remove (hash, optname))
				_notify_attributes (setting);
		}
		return;
	}

	v = g_hash_table_lookup (hash, optname);
	if (   v
	    && g_variant_is_of_type (v, G_VARIANT_TYPE_BOOLEAN)) {
		if (g_variant_get_boolean (v)) {
			if (value == NM_TERNARY_TRUE)
				return;
		} else {
			if (value == NM_TERNARY_FALSE)
				return;
		}
	}

	v = g_variant_ref_sink (g_variant_new_boolean (value != NM_TERNARY_FALSE));
	g_hash_table_insert (hash,
	                     g_strdup (optname),
	                     v);
	_notify_attributes (setting);
}

/**
 * nm_setting_ethtool_clear_offload_features:
 * @setting: the #NMSettingEthtool
 *
 * Clears all offload features settings
 *
 * Since: 1.14
 */
void
nm_setting_ethtool_clear_offload_features (NMSettingEthtool *setting)
{
	GHashTable *hash;
	GHashTableIter iter;
	const char *name;
	gboolean changed = FALSE;

	g_return_if_fail (NM_IS_SETTING_ETHTOOL (setting));

	hash = _nm_setting_gendata_hash (NM_SETTING (setting), FALSE);
	if (!hash)
		return;

	g_hash_table_iter_init (&iter, hash);
	while (g_hash_table_iter_next (&iter, (gpointer *) &name, NULL)) {
		if (nm_ethtool_optname_is_offload_feature (name)) {
			g_hash_table_iter_remove (&iter);
			changed = TRUE;
		}
	}

	if (changed)
		_notify_attributes (setting);
}

/*****************************************************************************/

/**
 * nm_setting_ethtool_offload_feature_from_string:
 * @setting: the #NMSettingEthtool
 *
 * Returns #NMSettingEthtool:offload-feature in a string representation.
 * This can be set again via nm_setting_ethtool_offload_feature_from_string().
 * Note, that it is possible to construct a #NMSettingEthtool connection
 * that contains invalid offload-feature settings. This causes the setting
 * to fail validation and such invalid settings are excluded, as they
 * have no meaningful string representation.
 *
 * Returns: a string representing all the valid offload features.
 *
 * Since: 1.14
 */
char *
nm_setting_ethtool_offload_feature_to_string (NMSettingEthtool *setting)
{
	GHashTable *hash;
	GString *str = NULL;
	GVariant *v;
	guint num;
	int i;

	g_return_val_if_fail (NM_IS_SETTING_ETHTOOL (setting), NULL);

	hash = _nm_setting_gendata_hash (NM_SETTING (setting), FALSE);

	num = hash ? g_hash_table_size (hash) : 0;

	for (i = _NM_ETHTOOL_ID_OFFLOAD_FEATURE_FIRST; num > 0 && i <= _NM_ETHTOOL_ID_OFFLOAD_FEATURE_LAST; i++) {
		const char *optname = nm_ethtool_data[i]->optname;

		v = g_hash_table_lookup (hash, optname);
		if (!v)
			continue;

		/* nm_ethtool_data has no duplicate names. We decrement @num to keep track
		 * of the unseen elements in @hash. If it reaches zero, we can abort early. */
		num--;

		if (!g_variant_is_of_type (v, G_VARIANT_TYPE_BOOLEAN))
			continue;

		if (str)
			str = g_string_new ("");
		else
			g_string_append_c (str, ' ');
		g_string_append (str, optname);
		g_string_append (str, g_variant_get_boolean (v) ? " on" : " off");
	}

	return str ? g_string_free (str, FALSE) : g_new0 (char, 1);
}

static const char *
_str_pop_arg (char **value)
{
	char *s0;
	char *s;

	nm_assert (value);

	s0 = *value;

	nm_assert (s0);
	nm_assert (s0[0] != '\0' && !g_ascii_isspace (s0[0]));

	for (s = &s0[1]; s[0]; s++) {
		if (!g_ascii_isspace (s[0]))
			continue;
		s[0] = '\0';
		for (s++; s[0] && g_ascii_isspace (s[0]); s++) {
			/*nop*/
		}
		break;
	}

	*value = s;
	return s0;
}

/**
 * nm_setting_ethtool_offload_feature_from_string:
 * @setting: the #NMSettingEthtool
 * @value: the offload-features encoded as a string
 * @best_effort: if @value contains invalid content, then it depends
 *   on @best_effort whether @setting will be changed. If %TRUE,
 *   this sets the properties that could be parsed. If %FALSE,
 *   @setting is unchanged in case of error.
 * @err_pos: (allow-none): (out): if not %NULL, this will point to the
 *   location in @value, where the first error occured. In case, @value
 *   is correct, this will be set to %NULL.
 * @error: (allow-none): location to store error information on failure, or %NULL
 *
 * Parses @value and sets all offload-feature accordingly. All previously
 * set offload-features are cleared.
 *
 * Returns: %TRUE in case @value could be successfully interpreted as a offload-feature
 *   setting. In that case, @error and @err_pos are set to %NULL.
 *
 * Since: 1.14
 */
gboolean
nm_setting_ethtool_offload_feature_from_string (NMSettingEthtool *setting,
                                                const char *value,
                                                gboolean best_effort,
                                                const char **err_pos,
                                                GError **error)
{
	//XXX
#if 0
	NMSettingEthtoolPrivate *priv;
	gs_unref_hashtable GHashTable *hash = NULL;
	const char *value_no_leading;
	gs_free char *value_clone = NULL;
	char *vv;
	gssize err_offset = -1;

	g_return_val_if_fail (NM_IS_SETTING_ETHTOOL (setting), FALSE);
	g_return_val_if_fail (value, FALSE);
	g_return_val_if_fail (!error || !*error, FALSE);

	priv = NM_SETTING_ETHTOOL_GET_PRIVATE (setting);

	value_no_leading = nm_str_skip_leading_spaces (value);
	if (!value_no_leading[0]) {
		NM_SET_OUT (err_pos, NULL);
		nm_setting_ethtool_clear_offload_features (setting);
		return TRUE;
	}

	value_clone = g_strdup (value_no_leading);
	vv = value_clone;

	for (vv = value_clone; vv[0]; ) {
		const char *s_name = NULL;
		const char *s_arg = NULL;
		NMEthtoolID offload_id;
		gboolean is_on;

		s_name = _str_pop_arg (&vv);

parse_sname_popped:

		nm_assert (s_name);

		offload_id = nm_ethtool_offload_id_from_name (s_name);

		if (offload_id == NM_ETHTOOL_ID_UNKNOWN) {
			/* invalid name. */
			if (err_offset < 0) {
				err_offset = s_name - value_clone;
				g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_ARGUMENTS,
				             _("unrecognized offload feature"));
				if (!best_effort)
					break;
			}
			/* although unrecognized, assume that the name is followed by on|off.
			 * Pop that first and continue parsing... */
		}

		if (vv[0])
			s_arg = _str_pop_arg (&vv);

		if (!s_arg) {
			nm_assert (vv[0] == '\0');

			/* lacks a second argument. We expect either "on" or "off". */
			if (err_offset < 0) {
				err_offset = s_name - value_clone;
				g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_ARGUMENTS,
				             _("missing argument \"on\" or \"off\""));
			}

			/* we are always done. It's the end of the line */
			break;
		}

		if (offload_id == NM_ETHTOOL_ID_UNKNOWN) {
			/* before, we already determined that @s_name is invalid. Now, we popped the
			 * following argument ... */
			if (NM_IN_SET (s_arg, "on", "off")) {
				/* it's just on/off. We successfully consumed it. Proceeed. */
				continue;
			}
			/* hm? s_arg is not on/off. Maybe it's itself a valid name. Backtrack. */
			s_name = s_arg;
			s_arg = NULL;
			goto parse_sname_popped;
		}

		if (nm_streq (s_arg, "on"))
			is_on = TRUE;
		else if (nm_streq (s_arg, "off"))
			is_on = FALSE;
		else {
			/* invalid s_arg. */
			if (err_offset < 0) {
				err_offset = s_arg - value_clone;
				g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_ARGUMENTS,
				             _("expect argument \"on\" or \"off\""));
				if (!best_effort)
					break;
			}
			continue;
		}

		if (!hash)
			hash = g_hash_table_new_full (nm_str_hash, g_str_equal, g_free, (GDestroyNotify) g_variant_unref);

		g_hash_table_insert (hash,
		                     g_strdup (nm_ethtool_offload_names[offload_id]),
		                     g_variant_ref_sink (g_variant_new_boolean (is_on)));
	}

	if (   (   err_offset < 0
	        || best_effort)
	    && !nm_utils_hash_table_equal (hash, priv->attributes, TRUE,
	                                   nm_gvariant_equal_with_data, NULL)) {
		if (hash) {
			g_hash_table_unref (priv->attributes);
			priv->attributes = hash;
		} else
			g_hash_table_remove_all (priv->attributes);
		_notify_attributes (setting);
	}

	NM_SET_OUT (err_pos,   err_offset < 0
	                     ? NULL
	                     : &value[err_offset]);
	return err_offset < 0;
#endif
	return FALSE;
}

/*****************************************************************************/

static GVariant *
_offload_feature_to_dbus (const GValue *from)
{
	GHashTable *hash;
	gs_free const char **keys = NULL;
	GVariantBuilder builder;
	guint i, len;

	//XXX

	/* FIXME: improve the D-Bus glue-code, so that during transform we don't need
	 * to clone the entire hash table for no reasons. */

	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

	hash = g_value_get_boxed (from);
	if (!hash)
		goto out;
	len = g_hash_table_size (hash);
	if (!len)
		goto out;

	keys = nm_utils_strdict_get_keys (hash,
	                                  TRUE,
	                                  NULL);
	for (i = 0; keys[i]; i++) {
		GVariant *v;

		v = g_hash_table_lookup (hash, keys[i]);
		g_variant_builder_add (&builder, "{sv}", keys[i], v);
	}

out:
	return g_variant_builder_end (&builder);
}

static void
_offload_feature_from_dbus (GVariant *from, GValue *to)
{
	GVariantIter iter;
	const char *key;
	GVariant *variant;
	GHashTable *hash;

	//XXX

	hash = g_hash_table_new_full (nm_str_hash, g_str_equal, g_free, (GDestroyNotify) g_variant_unref);
	g_variant_iter_init (&iter, from);
	while (g_variant_iter_next (&iter, "{&sv}", &key, &variant))
		g_hash_table_insert (hash, g_strdup (key), g_variant_ref (variant));

	g_value_take_boxed (to, hash);
}

/*****************************************************************************/

static gboolean
verify (NMSetting *setting, NMConnection *connection, GError **error)
{
	GHashTable *hash;
	GHashTableIter iter;
	const char *optname;
	GVariant *variant;

	hash = _nm_setting_gendata_hash (setting, FALSE);

	if (!hash)
		goto out;

	g_hash_table_iter_init (&iter, hash);
	while (g_hash_table_iter_next (&iter, (gpointer *) &optname, (gpointer *) &variant)) {
		if (!nm_ethtool_optname_is_offload_feature (optname)) {
			g_set_error (error,
			             NM_CONNECTION_ERROR,
			             NM_CONNECTION_ERROR_INVALID_PROPERTY,
			             _("unsupported offload feature '%s'"),
			             optname);
			g_prefix_error (error, "%s.%s: ", NM_SETTING_ETHTOOL_SETTING_NAME, NM_SETTING_ETHTOOL_OPTS);
			return FALSE;
		}
		if (!g_variant_is_of_type (variant, G_VARIANT_TYPE_BOOLEAN)) {
			g_set_error (error,
			             NM_CONNECTION_ERROR,
			             NM_CONNECTION_ERROR_INVALID_PROPERTY,
			             _("offload feature '%s' has invalid variant type"),
			             optname);
			g_prefix_error (error, "%s.%s: ", NM_SETTING_ETHTOOL_SETTING_NAME, NM_SETTING_ETHTOOL_OPTS);
			return FALSE;
		}
	}

out:
	return TRUE;
}

/*****************************************************************************/

static gboolean
compare_property (NMSetting *setting,
                  NMSetting *other,
                  const GParamSpec *prop_spec,
                  NMSettingCompareFlags flags)
{
	//XXX
#if 0
	if (nm_streq0 (prop_spec->name, NM_SETTING_ETHTOOL_OPTS)) {
		return nm_utils_hash_table_equal (NM_SETTING_ETHTOOL_GET_PRIVATE (setting)->attributes,
		                                  NM_SETTING_ETHTOOL_GET_PRIVATE (other)->attributes,
		                                  FALSE,
		                                  nm_gvariant_equal_with_data,
		                                  NULL);
	}
#endif

	return NM_SETTING_CLASS (nm_setting_ethtool_parent_class)->compare_property (setting, other, prop_spec, flags);
}

/*****************************************************************************/

static void
get_property (GObject *object, guint prop_id,
              GValue *value, GParamSpec *pspec)
{
	switch (prop_id) {
	case PROP_OPTS:
		_nm_setting_gendata_to_gvalue (NM_SETTING (object), value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
set_property (GObject *object, guint prop_id,
              const GValue *value, GParamSpec *pspec)
{
	switch (prop_id) {
	case PROP_OPTS:
		_nm_setting_gendata_reset_from_hash (NM_SETTING (object),
		                                     g_value_get_boxed (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/*****************************************************************************/

static void
nm_setting_ethtool_init (NMSettingEthtool *setting)
{
}

/**
 * nm_setting_ethtool_new:
 *
 * Creates a new #NMSettingEthtool object with default values.
 *
 * Returns: (transfer full): the new empty #NMSettingEthtool object
 *
 * Since: 1.14
 **/
NMSetting *
nm_setting_ethtool_new (void)
{
	return g_object_new (NM_TYPE_SETTING_ETHTOOL, NULL);
}

static void
nm_setting_ethtool_class_init (NMSettingEthtoolClass *self_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (self_class);
	NMSettingClass *setting_class = NM_SETTING_CLASS (self_class);

	object_class->set_property = set_property;
	object_class->get_property = get_property;

	setting_class->setting_info     = &nm_meta_setting_infos[NM_META_SETTING_TYPE_ETHTOOL];
	setting_class->verify           = verify;
	setting_class->compare_property = compare_property;

	/**
	 * NMSettingEthtool:opts: (type GHashTable(utf8,GVariant))
	 *
	 * Set of ethtool options. The accepted variant type depends
	 * on the option name (the key). Currently only options for
	 * offload features are supported. Offload feature options must
	 * have a boolean variant that indicates whether to enable/disable the
	 * feature.
	 *
	 * Since: 1.14
	 **/
	obj_properties[PROP_OPTS] =
	     g_param_spec_boxed (NM_SETTING_ETHTOOL_OPTS, "", "",
	                         G_TYPE_HASH_TABLE,
	                         G_PARAM_READWRITE |
	                         G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties (object_class, _PROPERTY_ENUMS_LAST, obj_properties);

	//XXX
	_nm_setting_class_transform_property (setting_class, NM_SETTING_ETHTOOL_OPTS,
	                                      G_VARIANT_TYPE ("a{sv}"),
	                                      _offload_feature_to_dbus,
	                                      _offload_feature_from_dbus);
}
