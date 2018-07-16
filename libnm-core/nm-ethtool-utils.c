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

#include "nm-ethtool-utils.h"

#include "nm-core-internal.h"

/*****************************************************************************/

#define ETHT_DATA(xname) \
	[NM_ETHTOOL_ID_##xname] = { \
	   .optname = NM_ETHTOOL_OPTNAME_##xname, \
	   .id = NM_ETHTOOL_ID_##xname, \
	}

static const NMEthtoolData _data[_NM_ETHTOOL_ID_NUM] = {
	ETHT_DATA (OFFLOAD_FEATURE_GRO),
	ETHT_DATA (OFFLOAD_FEATURE_GSO),
	ETHT_DATA (OFFLOAD_FEATURE_LRO),
	ETHT_DATA (OFFLOAD_FEATURE_NTUPLE),
	ETHT_DATA (OFFLOAD_FEATURE_RXHASH),
	ETHT_DATA (OFFLOAD_FEATURE_RXVLAN),
	ETHT_DATA (OFFLOAD_FEATURE_RX),
	ETHT_DATA (OFFLOAD_FEATURE_SG),
	ETHT_DATA (OFFLOAD_FEATURE_TSO),
	ETHT_DATA (OFFLOAD_FEATURE_TXVLAN),
	ETHT_DATA (OFFLOAD_FEATURE_TX),
};

#define ETHT_DATA_BY_ID(xname) \
	[NM_ETHTOOL_ID_##xname] = &_data[NM_ETHTOOL_ID_##xname]

const NMEthtoolData *const nm_ethtool_data[_NM_ETHTOOL_ID_NUM + 1] = {
	ETHT_DATA_BY_ID (OFFLOAD_FEATURE_GRO),
	ETHT_DATA_BY_ID (OFFLOAD_FEATURE_GSO),
	ETHT_DATA_BY_ID (OFFLOAD_FEATURE_LRO),
	ETHT_DATA_BY_ID (OFFLOAD_FEATURE_NTUPLE),
	ETHT_DATA_BY_ID (OFFLOAD_FEATURE_RXHASH),
	ETHT_DATA_BY_ID (OFFLOAD_FEATURE_RXVLAN),
	ETHT_DATA_BY_ID (OFFLOAD_FEATURE_RX),
	ETHT_DATA_BY_ID (OFFLOAD_FEATURE_SG),
	ETHT_DATA_BY_ID (OFFLOAD_FEATURE_TSO),
	ETHT_DATA_BY_ID (OFFLOAD_FEATURE_TXVLAN),
	ETHT_DATA_BY_ID (OFFLOAD_FEATURE_TX),
	[_NM_ETHTOOL_ID_NUM] = NULL,
};

#define ETHT_DATA_BY_NAME(xname) \
	&_data[NM_ETHTOOL_ID_##xname]

const NMEthtoolData *const _data_by_name[_NM_ETHTOOL_ID_NUM + 1] = {
	ETHT_DATA_BY_NAME (OFFLOAD_FEATURE_GRO),
	ETHT_DATA_BY_NAME (OFFLOAD_FEATURE_GSO),
	ETHT_DATA_BY_NAME (OFFLOAD_FEATURE_LRO),
	ETHT_DATA_BY_NAME (OFFLOAD_FEATURE_NTUPLE),
	ETHT_DATA_BY_NAME (OFFLOAD_FEATURE_RXHASH),
	ETHT_DATA_BY_NAME (OFFLOAD_FEATURE_RXVLAN),
	ETHT_DATA_BY_NAME (OFFLOAD_FEATURE_RX),
	ETHT_DATA_BY_NAME (OFFLOAD_FEATURE_SG),
	ETHT_DATA_BY_NAME (OFFLOAD_FEATURE_TSO),
	ETHT_DATA_BY_NAME (OFFLOAD_FEATURE_TXVLAN),
	ETHT_DATA_BY_NAME (OFFLOAD_FEATURE_TX),
	[_NM_ETHTOOL_ID_NUM] = NULL,
};

/*****************************************************************************/

static void
_ASSERT_data (void)
{
#if NM_MORE_ASSERTS > 10
	int i;

	G_STATIC_ASSERT_EXPR (_NM_ETHTOOL_ID_FIRST == 0);
	G_STATIC_ASSERT_EXPR (_NM_ETHTOOL_ID_LAST == _NM_ETHTOOL_ID_NUM - 1);
	G_STATIC_ASSERT_EXPR (_NM_ETHTOOL_ID_NUM > 0);

	nm_assert (G_N_ELEMENTS (_data)              == _NM_ETHTOOL_ID_NUM);
	nm_assert (NM_PTRARRAY_LEN (nm_ethtool_data) == _NM_ETHTOOL_ID_NUM);
	nm_assert (NM_PTRARRAY_LEN (_data_by_name)   == _NM_ETHTOOL_ID_NUM);
	nm_assert (G_N_ELEMENTS (nm_ethtool_data)    == _NM_ETHTOOL_ID_NUM + 1);
	nm_assert (G_N_ELEMENTS (_data_by_name)      == _NM_ETHTOOL_ID_NUM + 1);

	for (i = 0; i < _NM_ETHTOOL_ID_NUM; i++) {
		const NMEthtoolData *d = &_data[i];

		nm_assert (nm_ethtool_data[i] == d);
		nm_assert (d->id == (NMEthtoolID) i);
		nm_assert (d->optname && d->optname[0]);
	}

	for (i = 0; i < _NM_ETHTOOL_ID_NUM; i++) {
		const NMEthtoolData *d = _data_by_name[i];

		nm_assert (d >= &_data[_NM_ETHTOOL_ID_FIRST]);
		nm_assert (d <= &_data[_NM_ETHTOOL_ID_LAST]);
		nm_assert (((char *) d - (char *) &_data[0]) % sizeof (*d) == 0);
		if (i > 0)
			nm_assert (strcmp (_data_by_name[i - 1]->optname, d->optname) < 0);
	}
#endif
}

const NMEthtoolData *
nm_ethtool_data_get_by_optname (const char *optname)
{
	gssize idx;

	nm_assert (optname);

	_ASSERT_data ();

	G_STATIC_ASSERT_EXPR (G_STRUCT_OFFSET (NMEthtoolData, optname) == 0);

	idx = _nm_utils_ptrarray_find_binary_search ((gconstpointer *) _data_by_name,
	                                             _NM_ETHTOOL_ID_NUM,
	                                             &optname,
	                                             nm_strcmp_p_with_data,
	                                             NULL,
	                                             NULL,
	                                             NULL);

	return (idx < 0) ? NULL : &_data[idx];
}
