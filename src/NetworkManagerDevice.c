/* NetworkManager -- Network link manager
 *
 * Dan Williams <dcbw@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * (C) Copyright 2004 Red Hat, Inc.
 */

#include <errno.h>
#include <glib.h>
#include <dbus/dbus-glib.h>
#include <hal/libhal.h>
#include <iwlib.h>
#include <signal.h>
#include <string.h>

#include "NetworkManager.h"
#include "NetworkManagerMain.h"
#include "NetworkManagerDevice.h"
#include "NetworkManagerDevicePrivate.h"
#include "NetworkManagerUtils.h"
#include "NetworkManagerDbus.h"
#include "NetworkManagerWireless.h"
#include "NetworkManagerPolicy.h"
#include "NetworkManagerAPList.h"
#include "NetworkManagerSystem.h"
#include "NetworkManagerDHCP.h"

/* Local static prototypes */
static gboolean mii_get_link (NMDevice *dev);
static gpointer nm_device_worker (gpointer user_data);
static gboolean nm_device_activate (gpointer user_data);
static gboolean nm_device_activation_configure_ip (NMDevice *dev, gboolean do_only_autoip);
static gboolean nm_device_wireless_scan (gpointer user_data);

typedef struct
{
	NMDevice					*dev;
	struct wireless_scan_head	 scan_head;
} NMWirelessScanResults;


/******************************************************/


/******************************************************/

/*
 * nm_device_test_wireless_extensions
 *
 * Test whether a given device is a wireless one or not.
 *
 */
static gboolean nm_device_test_wireless_extensions (NMDevice *dev)
{
	int		sk;
	int		err;
	char		ioctl_buf[64];
	
	g_return_val_if_fail (dev != NULL, FALSE);

	/* We obviously cannot probe test devices (since they don't
	 * actually exist in hardware).
	 */
	if (dev->test_device)
		return (FALSE);

	ioctl_buf[63] = 0;
	strncpy(ioctl_buf, nm_device_get_iface(dev), 63);

	sk = iw_sockets_open ();
	err = ioctl(sk, SIOCGIWNAME, ioctl_buf);
	close (sk);
	return (err == 0);
}


/*
 * nm_device_supports_wireless_scan
 *
 * Test whether a given device is a wireless one or not.
 *
 */
static gboolean nm_device_supports_wireless_scan (NMDevice *dev)
{
	int				sk;
	int				err;
	gboolean			can_scan = TRUE;
	wireless_scan_head	scan_data;
	
	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (dev->type == DEVICE_TYPE_WIRELESS_ETHERNET, FALSE);

	/* A test wireless device can always scan (we generate fake scan data for it) */
	if (dev->test_device)
		return (TRUE);
	
	sk = iw_sockets_open ();
	err = iw_scan (sk, (char *)nm_device_get_iface (dev), WIRELESS_EXT, &scan_data);
	nm_dispose_scan_results (scan_data.result);
	if ((err == -1) && (errno == EOPNOTSUPP))
		can_scan = FALSE;
	close (sk);
	return (can_scan);
}


/*
 * nm_get_device_by_udi
 *
 * Search through the device list for a device with a given UDI.
 *
 * NOTE: the caller MUST hold the device list mutex already to make
 * this routine thread-safe.
 *
 */
NMDevice *nm_get_device_by_udi (NMData *data, const char *udi)
{
	NMDevice	*dev = NULL;
	GSList	*element;
	
	g_return_val_if_fail (data != NULL, NULL);
	g_return_val_if_fail (udi  != NULL, NULL);

	element = data->dev_list;
	while (element)
	{
		dev = (NMDevice *)(element->data);
		if (dev)
		{
			if (nm_null_safe_strcmp (nm_device_get_udi (dev), udi) == 0)
				break;
		}

		element = g_slist_next (element);
	}

	return (dev);
}


/*
 * nm_get_device_by_iface
 *
 * Search through the device list for a device with a given iface.
 *
 * NOTE: the caller MUST hold the device list mutex already to make
 * this routine thread-safe.
 *
 */
NMDevice *nm_get_device_by_iface (NMData *data, const char *iface)
{
	NMDevice	*iter_dev = NULL;
	NMDevice	*found_dev = NULL;
	GSList	*element;
	
	g_return_val_if_fail (data  != NULL, NULL);
	g_return_val_if_fail (iface != NULL, NULL);

	element = data->dev_list;
	while (element)
	{
		iter_dev = (NMDevice *)(element->data);
		if (iter_dev)
		{
			if (nm_null_safe_strcmp (nm_device_get_iface (iter_dev), iface) == 0)
			{
				found_dev = iter_dev;
				break;
			}
		}

		element = g_slist_next (element);
	}

	return (found_dev);
}


/*****************************************************************************/
/* NMDevice object routines                                                  */
/*****************************************************************************/

/*
 * nm_device_new
 *
 * Creates and initializes the structure representation of an NM device.  For test
 * devices, a device type other than DEVICE_TYPE_DONT_KNOW must be specified, this
 * argument is ignored for real hardware devices since they are auto-probed.
 *
 */
NMDevice *nm_device_new (const char *iface, const char *udi, gboolean test_dev, NMDeviceType test_dev_type, NMData *app_data)
{
	NMDevice	*dev;
	GError	*error = NULL;

	g_return_val_if_fail (iface != NULL, NULL);
	g_return_val_if_fail (strlen (iface) > 0, NULL);
	g_return_val_if_fail (app_data != NULL, NULL);

	/* Test devices must have a valid type specified */
	if (test_dev && !(test_dev_type != DEVICE_TYPE_DONT_KNOW))
		return (NULL);

	/* Another check to make sure we don't create a test device unless
	 * test devices were enabled on the command line.
	 */
	if (!app_data->enable_test_devices && test_dev)
	{
		syslog (LOG_ERR, "nm_device_new(): attempt to create a test device, but test devices were not enabled"
					" on the command line.  Will not create the device.\n");
		return (NULL);
	}

	dev = g_new0 (NMDevice, 1);
	if (!dev)
	{
		syslog (LOG_ERR, "nm_device_new() could not allocate a new device...  Not enough memory?");
		return (NULL);
	}

	dev->refcount = 2; /* 1 for starters, and another 1 for the worker thread */
	dev->app_data = app_data;
	dev->iface = g_strdup (iface);
	dev->test_device = test_dev;
	nm_device_set_udi (dev, udi);

	/* Real hardware devices are probed for their type, test devices must have
	 * their type specified.
	 */
	if (test_dev)
		dev->type = test_dev_type;
	else
		dev->type = nm_device_test_wireless_extensions (dev) ?
						DEVICE_TYPE_WIRELESS_ETHERNET : DEVICE_TYPE_WIRED_ETHERNET;

	/* Have to bring the device up before checking link status and other stuff */
	nm_device_bring_up (dev);

	/* Initialize wireless-specific options */
	if (nm_device_is_wireless (dev))
	{
		iwrange	range;
		int		sk;

		dev->options.wireless.scan_interval = 20;

		if (!(dev->options.wireless.scan_mutex = g_mutex_new ()))
		{
			g_free (dev->iface);
			g_free (dev);
			return (NULL);
		}

		if (!(dev->options.wireless.best_ap_mutex = g_mutex_new ()))
		{
			g_mutex_free (dev->options.wireless.scan_mutex);
			g_free (dev->iface);
			g_free (dev);
			return (NULL);
		}

		if (!(dev->options.wireless.ap_list = nm_ap_list_new (NETWORK_TYPE_DEVICE)))
		{
			g_free (dev->iface);
			g_mutex_free (dev->options.wireless.best_ap_mutex);
			g_free (dev);
			return (NULL);
		}
		dev->options.wireless.supports_wireless_scan = nm_device_supports_wireless_scan (dev);

		nm_device_set_mode (dev, NETWORK_MODE_INFRA);

		if ((sk = iw_sockets_open ()) >= 0)
		{
			if (iw_get_range_info (sk, nm_device_get_iface (dev), &(dev->options.wireless.range_info)) == -1)
				memset (&(dev->options.wireless.range_info), 0, sizeof (struct iw_range));
			close (sk);
		}
	}

	dev->driver_support_level = nm_get_driver_support_level (dev->app_data->hal_ctx, dev);

	if (nm_device_get_driver_support_level (dev) != NM_DRIVER_UNSUPPORTED)
	{
		nm_device_update_link_active (dev, TRUE);

		nm_device_update_ip4_address (dev);
		nm_device_update_hw_address (dev);

		/* Grab IP config data for this device from the system configuration files */
		nm_system_device_update_config_info (dev);
	}

	if (!g_thread_create (nm_device_worker, dev, FALSE, &error))
	{
		syslog (LOG_CRIT, "nm_device_new (): could not create device worker thread. (glib said: '%s')", error->message);
		g_error_free (error);

		/* When we get here, we've got a refcount of 2, one because the
		 * device starts off with a refcount of 1, and a second ref because
		 * so that it sticks around for the worker thread.  So we have to unref twice.
		 */
		nm_device_unref (dev);
		nm_device_unref (dev);
		dev = NULL;
	}

	/* Block until our device thread has actually had a chance to start. */
	syslog (LOG_ERR, "nm_device_new(): waiting for device's worker thread to start.\n");
	while (dev->worker_started == FALSE)
		g_usleep (G_USEC_PER_SEC / 2);
	syslog (LOG_ERR, "nm_device_new(): device's worker thread started, continuing.\n");

	return (dev);
}


/*
 * Refcounting functions
 */
void nm_device_ref (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);

	dev->refcount++;
}

/*
 * nm_device_unref
 *
 * Decreases the refcount on a device by 1, and if the refcount reaches 0,
 * deallocates memory used by the device.
 *
 * Returns:	FALSE if device was not deallocated
 *			TRUE if device was deallocated
 */
gboolean nm_device_unref (NMDevice *dev)
{
	gboolean	deleted = FALSE;

	g_return_val_if_fail (dev != NULL, TRUE);

	dev->refcount--;
	if (dev->refcount <= 0)
	{
		if (dev->loop)
			g_main_loop_quit (dev->loop);
		while (dev->worker_done == FALSE)
			g_usleep (300);

		if (nm_device_is_wireless (dev))
		{
			nm_device_ap_list_clear (dev);
			dev->options.wireless.ap_list = NULL;

			g_mutex_free (dev->options.wireless.scan_mutex);
			if (dev->options.wireless.ap_list)
				nm_ap_list_unref (dev->options.wireless.ap_list);
			if (dev->options.wireless.best_ap)
				nm_ap_unref (dev->options.wireless.best_ap);
			g_mutex_free (dev->options.wireless.best_ap_mutex);
		}

		g_free (dev->udi);
		dev->udi = NULL;
		g_free (dev->iface);
		dev->iface = NULL;
		g_free (dev);
		deleted = TRUE;
	}

	return deleted;
}


/*
 * nm_device_worker
 *
 * Main thread of the device.
 *
 */
static gpointer nm_device_worker (gpointer user_data)
{
	NMDevice *dev = (NMDevice *)user_data;

	if (!dev)
	{
		syslog (LOG_CRIT, "nm_device_worker(): received NULL device object, NetworkManager cannot continue.\n");
		exit (1);
	}

	dev->context = g_main_context_new ();
	dev->loop = g_main_loop_new (dev->context, FALSE);

	dev->worker_started = TRUE;

	/* Do an initial wireless scan */
	if (nm_device_is_wireless (dev))
	{
		GSource	*source = g_idle_source_new ();
		guint	 source_id = 0;

		g_source_set_callback (source, nm_device_wireless_scan, dev, NULL);
		source_id = g_source_attach (source, dev->context);
		g_source_unref (source);
	}

	g_main_loop_run (dev->loop);

	if (nm_device_config_get_use_dhcp (dev))
	{
		if (dev->renew_timeout > 0)
			g_source_remove (dev->renew_timeout);
		if (dev->rebind_timeout > 0)
			g_source_remove (dev->rebind_timeout);
	}

	g_main_loop_unref (dev->loop);
	g_main_context_unref (dev->context);

	dev->loop = NULL;
	dev->context = NULL;
	dev->renew_timeout = 0;
	dev->rebind_timeout = 0;

	dev->worker_done = TRUE;
	nm_device_unref (dev);

	return NULL;
}


void nm_device_worker_thread_stop (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);

	g_main_loop_quit (dev->loop);
	while (dev->worker_done == FALSE)
		g_usleep (G_USEC_PER_SEC / 2);
}


/*
 * nm_device_get_app_data
 *
 */
NMData *nm_device_get_app_data (const NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	return (dev->app_data);
}


/*
 * Get/Set for "removed" flag
 */
gboolean nm_device_get_removed (const NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, TRUE);

	return (dev->removed);
}

void nm_device_set_removed (NMDevice *dev, const gboolean removed)
{
	g_return_if_fail (dev != NULL);

	dev->removed = removed;
}


/*
 * nm_device_open_sock
 *
 * Get a control socket for network operations.
 *
 */
int nm_device_open_sock (void)
{
	int	fd;

	/* Try to grab a control socket */
	fd = socket(PF_INET, SOCK_DGRAM, 0);
	if (fd >= 0)
	     return (fd);
	fd = socket(PF_PACKET, SOCK_DGRAM, 0);
	if (fd >= 0)
	     return (fd);
	fd = socket(PF_INET6, SOCK_DGRAM, 0);
	if (fd >= 0)
	     return (fd);

	syslog (LOG_ERR, "nm_get_network_control_socket() could not get network control socket.");
	return (-1);
}


/*
 * Return the amount of time we should wait for the device
 * to get a link, based on the # of frequencies it has to
 * scan.
 */
gint nm_device_get_association_pause_value (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, -1);
	g_return_val_if_fail (nm_device_is_wireless (dev), -1);

	/* If the card supports more than 14 channels, we should probably wait
	 * around 10s so it can scan them all. After we set the ESSID on the card, the card
	 * has to scan all channels to find our requested AP (which can take a long time
	 * if it is an A/B/G chipset like the Atheros 5212, for example).
	 */
	if (dev->options.wireless.range_info.num_frequency > 14)
		return 10;
	else
		return 5;
}


/*
 * Get/set functions for UDI
 */
char * nm_device_get_udi (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, NULL);

	return (dev->udi);
}

void nm_device_set_udi (NMDevice *dev, const char *udi)
{
	g_return_if_fail (dev != NULL);
	g_return_if_fail (udi != NULL);

	if (dev->udi)
		g_free (dev->udi);

	dev->udi = g_strdup (udi);
}


/*
 * Get/set functions for iface
 */
const char * nm_device_get_iface (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, NULL);

	return (dev->iface);
}


/*
 * Get/set functions for type
 */
guint nm_device_get_type (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, DEVICE_TYPE_DONT_KNOW);

	return (dev->type);
}

gboolean nm_device_is_wireless (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	return (dev->type == DEVICE_TYPE_WIRELESS_ETHERNET);
}

gboolean nm_device_is_wired (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	return (dev->type == DEVICE_TYPE_WIRED_ETHERNET);
}


/*
 * Accessor for driver support level
 */
NMDriverSupportLevel nm_device_get_driver_support_level (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, NM_DRIVER_UNSUPPORTED);

	return (dev->driver_support_level);
}


/*
 * Get/set functions for link_active
 */
gboolean nm_device_get_link_active (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	return (dev->link_active);
}

void nm_device_set_link_active (NMDevice *dev, const gboolean link_active)
{
	g_return_if_fail (dev != NULL);

	dev->link_active = link_active;
}


/*
 * Get/set functions for now_scanning
 */
gboolean nm_device_get_now_scanning (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (nm_device_is_wireless (dev), FALSE);

	return (dev->options.wireless.now_scanning);
}

void nm_device_set_now_scanning (NMDevice *dev, const gboolean now_scanning)
{
	gboolean	old_val;

	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	old_val = nm_device_get_now_scanning (dev);
	dev->options.wireless.now_scanning = now_scanning;
	if (old_val != now_scanning)
		nm_dbus_schedule_device_status_change (dev, DEVICE_STATUS_CHANGE);
}


/*
 * Get function for supports_wireless_scan
 */
gboolean nm_device_get_supports_wireless_scan (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	if (!nm_device_is_wireless (dev))
		return (FALSE);

	return (dev->options.wireless.supports_wireless_scan);
}


/*
 * nm_device_wireless_is_associated
 *
 * Figure out whether or not we're associated to an access point
 */
static gboolean nm_device_wireless_is_associated (NMDevice *dev)
{
	struct iwreq	wrq;
	int			sk;
	gboolean		associated = FALSE;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (dev->app_data != NULL, FALSE);

	/* Test devices have their link state set through DBUS */
	if (dev->test_device)
		return (nm_device_get_link_active (dev));

	if ((sk = iw_sockets_open ()) < 0)
		return (FALSE);

	/* Some cards, for example ipw2x00 cards, can short-circuit the MAC
	 * address check using this check on IWNAME.  Its faster.
	 */
	if (iw_get_ext (sk, nm_device_get_iface (dev), SIOCGIWNAME, &wrq) >= 0)
	{
		if (!strcmp(wrq.u.name, "unassociated"))
		{
			associated = FALSE;
			goto out;
		}
	}

	if (!associated)
	{
		/*
		 * For all other wireless cards, the best indicator of a "link" at this time
		 * seems to be whether the card has a valid access point MAC address.
		 * Is there a better way?  Some cards don't work too well with this check, ie
		 * Lucent WaveLAN.
		 */
		if (iw_get_ext (sk, nm_device_get_iface (dev), SIOCGIWAP, &wrq) >= 0)
			if (nm_ethernet_address_is_valid ((struct ether_addr *)(&(wrq.u.ap_addr.sa_data))))
				associated = TRUE;
	}

out:
	close (sk);

	return (associated);
}

/*
 * nm_device_wireless_link_active
 *
 * Gets the link state of a wireless device
 *
 */
static gboolean nm_device_wireless_link_active (NMDevice *dev)
{
	gboolean 		 link = FALSE;
	NMAccessPoint	*best_ap;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (dev->app_data != NULL, FALSE);

	/* Test devices have their link state set through DBUS */
	if (dev->test_device)
		return (nm_device_get_link_active (dev));

	if (!nm_device_wireless_is_associated (dev))
		return (FALSE);

	/* If we don't have a "best" ap, we can't logically have a valid link
	 * that we want to use.
	 */
	if ((best_ap = nm_device_get_best_ap (dev)))
	{
		if (!nm_device_need_ap_switch (dev))
			link = TRUE;
		nm_ap_unref (best_ap);
	}

	return (link);
}


/*
 * nm_device_wired_link_active
 *
 * Return the link state of a wired device.  We usually just grab the HAL
 * net.80203.link property, but on card insertion we need to check the MII
 * registers of the card to get a more accurate response, since HAL may not
 * have received a netlink socket link event for the device yet, and therefore
 * will return FALSE when the device really does have a link.
 *
 */
static gboolean nm_device_wired_link_active (NMDevice *dev, gboolean check_mii)
{
	gboolean	link = FALSE;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (dev->app_data != NULL, FALSE);

	/* Test devices have their link state set through DBUS */
	if (dev->test_device)
		return (nm_device_get_link_active (dev));

	if (check_mii)
		link = mii_get_link (dev);
	else if (hal_device_property_exists (dev->app_data->hal_ctx, nm_device_get_udi (dev), "net.80203.link"))
		link = hal_device_get_property_bool (dev->app_data->hal_ctx, nm_device_get_udi (dev), "net.80203.link");

	return (link);
}


/*
 * nm_device_update_link_active
 *
 * Updates the link state for a particular device.
 *
 */
void nm_device_update_link_active (NMDevice *dev, gboolean check_mii)
{
	gboolean		link = FALSE;

	g_return_if_fail (dev != NULL);
	g_return_if_fail (dev->app_data != NULL);

	switch (nm_device_get_type (dev))
	{
		case DEVICE_TYPE_WIRELESS_ETHERNET:
			link = nm_device_wireless_link_active (dev);
			/* Update our current signal strength too */
			nm_device_update_signal_strength (dev);
			break;

		case DEVICE_TYPE_WIRED_ETHERNET:
			link = nm_device_wired_link_active (dev, check_mii);
			break;

		default:
			link = nm_device_get_link_active (dev);	/* Can't get link info for this device, so don't change link status */
			break;
	}

	/* Update device link status and global state variable if the status changed */
	if (link != nm_device_get_link_active (dev))
	{
		nm_device_set_link_active (dev, link);
		nm_dbus_schedule_device_status_change (dev, DEVICE_STATUS_CHANGE);
		nm_policy_schedule_state_update (dev->app_data);
	}
}


/*
 * nm_device_get_essid
 *
 * If a device is wireless, return the essid that it is attempting
 * to use.
 *
 * Returns:	allocated string containing essid.  Must be freed by caller.
 *
 */
char * nm_device_get_essid (NMDevice *dev)
{
	int	sk;
	int	err;
	
	g_return_val_if_fail (dev != NULL, NULL);
	g_return_val_if_fail (nm_device_is_wireless (dev), NULL);

	/* Test devices return the essid of their "best" access point
	 * or if there is none, the contents of the cur_essid field.
	 */
	if (dev->test_device)
	{
		NMAccessPoint	*best_ap = nm_device_get_best_ap (dev);
		char			*essid = dev->options.wireless.cur_essid;

		/* Or, if we've got a best ap, use that ESSID instead */
		if (best_ap)
		{
			essid = nm_ap_get_essid (best_ap);
			nm_ap_unref (best_ap);
		}
		return (essid);
	}
	
	sk = iw_sockets_open ();
	if (sk >= 0)
	{
		wireless_config	info;

		err = iw_get_basic_config(sk, nm_device_get_iface (dev), &info);
		if (err >= 0)
		{
			if (dev->options.wireless.cur_essid)
				g_free (dev->options.wireless.cur_essid);
			dev->options.wireless.cur_essid = g_strdup (info.essid);
		}
		else
			syslog (LOG_ERR, "nm_device_get_essid(): error getting ESSID for device %s.  errno = %d", nm_device_get_iface (dev), errno);

		close (sk);
	}

	return (dev->options.wireless.cur_essid);
}


/*
 * nm_device_set_essid
 *
 * If a device is wireless, set the essid that it should use.
 */
void nm_device_set_essid (NMDevice *dev, const char *essid)
{
	int				sk;
	int				err;
	struct iwreq		wreq;
	unsigned char		safe_essid[IW_ESSID_MAX_SIZE + 1] = "\0";
	
	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	/* Test devices directly set cur_essid */
	if (dev->test_device)
	{
		if (dev->options.wireless.cur_essid)
			g_free (dev->options.wireless.cur_essid);
		dev->options.wireless.cur_essid = g_strdup (essid);
		return;
	}

	/* Make sure the essid we get passed is a valid size */
	if (!essid)
		safe_essid[0] = '\0';
	else
	{
		strncpy (safe_essid, essid, IW_ESSID_MAX_SIZE);
		safe_essid[IW_ESSID_MAX_SIZE] = '\0';
	}

	sk = iw_sockets_open ();
	if (sk >= 0)
	{
		wreq.u.essid.pointer = (caddr_t) safe_essid;
		wreq.u.essid.length	 = strlen (safe_essid) + 1;
		wreq.u.essid.flags	 = 1;	/* Enable essid on card */
	
		err = iw_set_ext (sk, nm_device_get_iface (dev), SIOCSIWESSID, &wreq);
		if (err == -1)
			syslog (LOG_ERR, "nm_device_set_essid(): error setting ESSID '%s' for device %s.  errno = %d", safe_essid, nm_device_get_iface (dev), errno);

		close (sk);
	}
}


/*
 * nm_device_get_frequency
 *
 * For wireless devices, get the frequency we broadcast/receive on.
 *
 */
double nm_device_get_frequency (NMDevice *dev)
{
	int		sk;
	int		err;
	double	freq = 0;

	g_return_val_if_fail (dev != NULL, 0);
	g_return_val_if_fail (nm_device_is_wireless (dev), 0);

	/* Test devices don't really have a frequency, they always succeed */
	if (dev->test_device)
		return 703000000;

	sk = iw_sockets_open ();
	if (sk >= 0)
	{
		struct iwreq		wrq;

		err = iw_set_ext (sk, nm_device_get_iface (dev), SIOCGIWFREQ, &wrq);
		if (err >= 0)
			freq = iw_freq2float (&wrq.u.freq);
		if (err == -1)
			syslog (LOG_ERR, "nm_device_get_frequency(): error getting frequency for device %s.  errno = %d", nm_device_get_iface (dev), errno);

		close (sk);
	}
	return (freq);
}


/*
 * nm_device_set_frequency
 *
 * For wireless devices, set the frequency to broadcast/receive on.
 * A frequency <= 0 means "auto".
 *
 */
void nm_device_set_frequency (NMDevice *dev, const double freq)
{
	int				sk;
	int				err;
	
	/* HACK FOR NOW */
	if (freq <= 0)
		return;

	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	/* Test devices don't really have a frequency, they always succeed */
	if (dev->test_device)
		return;

	if (nm_device_get_frequency (dev) == freq)
		return;

	sk = iw_sockets_open ();
	if (sk >= 0)
	{
		struct iwreq		wrq;

		if (freq <= 0)
		{
			/* Auto */
			/* People like to make things hard for us.  Even though iwlib/iwconfig say
			 * that wrq.u.freq.m should be -1 for "auto" mode, nobody actually supports
			 * that.  Madwifi actually uses "0" to mean "auto".  So, we'll try 0 first
			 * and if that doesn't work, fall back to the iwconfig method and use -1.
			 *
			 * As a further note, it appears that Atheros/Madwifi cards can't go back to
			 * any-channel operation once you force set the channel on them.  For example,
			 * if you set a prism54 card to a specific channel, but then set the ESSID to
			 * something else later, it will scan for the ESSID and switch channels just fine.
			 * Atheros cards, however, just stay at the channel you previously set and don't
			 * budge, no matter what you do to them, until you tell them to go back to
			 * any-channel operation.
			 */
			wrq.u.freq.m = 0;
			wrq.u.freq.e = 0;
			wrq.u.freq.flags = 0;
		}
		else
		{
			/* Fixed */
			wrq.u.freq.flags = IW_FREQ_FIXED;
			iw_float2freq (freq, &wrq.u.freq);
		}
		err = iw_set_ext (sk, nm_device_get_iface (dev), SIOCSIWFREQ, &wrq);
		if (err == -1)
		{
			gboolean	success = FALSE;
			if ((freq <= 0) && ((errno == EINVAL) || (errno == EOPNOTSUPP)))
			{
				/* Ok, try "auto" the iwconfig way if the Atheros way didn't work */
				wrq.u.freq.m = -1;
				wrq.u.freq.e = 0;
				wrq.u.freq.flags = 0;
				if (iw_set_ext (sk, nm_device_get_iface (dev), SIOCSIWFREQ, &wrq) != -1)
					success = TRUE;
			}
		}

		close (sk);
	}
}


/*
 * nm_device_get_bitrate
 *
 * For wireless devices, get the bitrate to broadcast/receive at.
 * Returned value is rate in KHz.
 *
 */
int nm_device_get_bitrate (NMDevice *dev)
{
	int				sk;
	int				err = -1;
	struct iwreq		wrq;
	
	g_return_val_if_fail (dev != NULL, 0);
	g_return_val_if_fail (nm_device_is_wireless (dev), 0);

	/* Test devices don't really have a frequency, they always succeed */
	if (dev->test_device)
		return 11;

	sk = iw_sockets_open ();
	if (sk >= 0)
	{
		err = iw_set_ext (sk, nm_device_get_iface (dev), SIOCGIWRATE, &wrq);
		close (sk);
	}

	return ((err >= 0) ? wrq.u.bitrate.value / 1000 : 0);
}


/*
 * nm_device_set_bitrate
 *
 * For wireless devices, set the bitrate to broadcast/receive at.
 * Rate argument should be in Mbps (mega-bits per second), or 0 for automatic.
 *
 */
void nm_device_set_bitrate (NMDevice *dev, const int Mbps)
{
	int				sk;
	int				err;
	
	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	/* Test devices don't really have a bitrate, they always succeed */
	if (dev->test_device)
		return;

	if (nm_device_get_bitrate (dev) == Mbps)
		return;

	sk = iw_sockets_open ();
	if (sk >= 0)
	{
		struct iwreq		wrq;

		if (Mbps != 0)
		{
			wrq.u.bitrate.value = Mbps * 1000;
			wrq.u.bitrate.fixed = 1;
		}
		else
		{
			/* Auto bitrate */
			wrq.u.bitrate.value = -1;
			wrq.u.bitrate.fixed = 0;
		}
		/* Silently fail as not all drivers support setting bitrate yet (ipw2x00 for example) */
		iw_set_ext (sk, nm_device_get_iface (dev), SIOCSIWRATE, &wrq);

		close (sk);
	}
}


/*
 * nm_device_get_ap_address
 *
 * If a device is wireless, get the access point's ethernet address
 * that the card is associated with.
 */
void nm_device_get_ap_address (NMDevice *dev, struct ether_addr *addr)
{
	int			iwlib_socket;
	struct iwreq	wrq;

	g_return_if_fail (dev != NULL);
	g_return_if_fail (addr != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	/* Test devices return an invalid address when there's no link,
	 * and a made-up address when there is a link.
	 */
	if (dev->test_device)
	{
		struct ether_addr	good_addr = { {0x70, 0x37, 0x03, 0x70, 0x37, 0x03} };
		struct ether_addr	bad_addr = { {0x00, 0x00, 0x00, 0x00, 0x00, 0x00} };
		gboolean			link = nm_device_get_link_active (dev);

		memcpy ((link ? &good_addr : &bad_addr), &(wrq.u.ap_addr.sa_data), sizeof (struct ether_addr));
		return;
	}

	iwlib_socket = iw_sockets_open ();
	if (iw_get_ext (iwlib_socket, nm_device_get_iface (dev), SIOCGIWAP, &wrq) >= 0)
		memcpy (addr, &(wrq.u.ap_addr.sa_data), sizeof (struct ether_addr));
	else
		memset (addr, 0, sizeof (struct ether_addr));
	close (iwlib_socket);
}


/*
 * nm_device_set_enc_key
 *
 * If a device is wireless, set the encryption key that it should use.
 *
 * key:	encryption key to use, or NULL or "" to disable encryption.
 *		NOTE that at this time, the key must be the raw HEX key, not
 *		a passphrase.
 */
void nm_device_set_enc_key (NMDevice *dev, const char *key, NMDeviceAuthMethod auth_method)
{
	int				sk;
	int				err;
	struct iwreq		wreq;
	int				keylen;
	unsigned char		safe_key[IW_ENCODING_TOKEN_MAX + 1];
	gboolean			set_key = FALSE;
	
	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	/* Test devices just ignore encryption keys */
	if (dev->test_device)
		return;

	/* Make sure the essid we get passed is a valid size */
	if (!key)
		safe_key[0] = '\0';
	else
	{
		strncpy (safe_key, key, IW_ENCODING_TOKEN_MAX);
		safe_key[IW_ENCODING_TOKEN_MAX] = '\0';
	}

	sk = iw_sockets_open ();
	if (sk >= 0)
	{
		wreq.u.data.pointer = (caddr_t) NULL;
		wreq.u.data.length = 0;
		wreq.u.data.flags = IW_ENCODE_ENABLED;

		/* Unfortunately, some drivers (Cisco) don't make a distinction between
		 * Open System authentication mode and whether or not to use WEP.  You
		 * DON'T have to use WEP when using Open System, but these cards force
		 * it.  Therefore, we have to set Open System mode when using WEP.
		 */

		if (strlen (safe_key) == 0)
		{
			wreq.u.data.flags |= IW_ENCODE_DISABLED | IW_ENCODE_NOKEY;
			set_key = TRUE;
		}
		else
		{
			unsigned char		parsed_key[IW_ENCODING_TOKEN_MAX + 1];

			keylen = iw_in_key_full (sk, nm_device_get_iface (dev), safe_key, &parsed_key[0], &wreq.u.data.flags);
			if (keylen > 0)
			{
				switch (auth_method)
				{
					case NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM:
						wreq.u.data.flags |= IW_ENCODE_OPEN;
						break;
					case NM_DEVICE_AUTH_METHOD_SHARED_KEY:
						wreq.u.data.flags |= IW_ENCODE_RESTRICTED;
						break;
					default:
						wreq.u.data.flags |= IW_ENCODE_RESTRICTED;
						break;
				}
				wreq.u.data.pointer	=  (caddr_t) &parsed_key;
				wreq.u.data.length	=  keylen;
				set_key = TRUE;
			}
		}

		if (set_key)
		{
			err = iw_set_ext (sk, nm_device_get_iface (dev), SIOCSIWENCODE, &wreq);
			if (err == -1)
				syslog (LOG_ERR, "nm_device_set_enc_key(): error setting key for device %s.  errno = %d", nm_device_get_iface (dev), errno);
		}

		close (sk);
	} else syslog (LOG_ERR, "nm_device_set_enc_key(): could not get wireless control socket.");
}


/*
 * nm_device_get_signal_strength
 *
 * Get the current signal strength of a wireless device.  This only works when
 * the card is associated with an access point, so will only work for the
 * active device.
 *
 * Returns:	-1 on error
 *			0 - 100  strength percentage of the connection to the current access point
 *
 */
gint8 nm_device_get_signal_strength (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, -1);
	g_return_val_if_fail (nm_device_is_wireless (dev), -1);

	return (dev->options.wireless.strength);
}


/*
 * nm_device_update_signal_strength
 *
 * Update the device's idea of the strength of its connection to the
 * current access point.
 *
 */
void nm_device_update_signal_strength (NMDevice *dev)
{
	gboolean	has_range;
	int		sk;
	iwrange	range;
	iwstats	stats;
	int		percent = -1;

	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));
	g_return_if_fail (dev->app_data != NULL);

	/* If we aren't the active device, we don't really have a signal strength
	 * that would mean anything.
	 */
	if (dev != dev->app_data->active_device)
	{
		dev->options.wireless.strength = -1;
		return;
	}

	/* Fake a value for test devices */
	if (dev->test_device)
	{
		dev->options.wireless.strength = 75;
		return;
	}

	sk = iw_sockets_open ();
	has_range = (iw_get_range_info (sk, nm_device_get_iface (dev), &range) >= 0);
	if (iw_get_stats (sk, nm_device_get_iface (dev), &stats, &range, has_range) == 0)
	{
		/* Update our max quality while we're at it */
		dev->options.wireless.max_quality = range.max_qual.level;
		dev->options.wireless.noise = stats.qual.noise;
		percent = nm_wireless_qual_to_percent (dev, &(stats.qual));
	}
	else
	{
		dev->options.wireless.max_quality = -1;
		dev->options.wireless.noise = -1;
		percent = -1;
	}
	close (sk);

	/* Try to smooth out the strength.  Atmel cards, for example, will give no strength
	 * one second and normal strength the next.
	 */
	if ((percent == -1) && (++dev->options.wireless.invalid_strength_counter <= 3))
		percent = dev->options.wireless.strength;
	else
		dev->options.wireless.invalid_strength_counter = 0;

	dev->options.wireless.strength = percent;
}


/*
 * nm_device_get_noise
 *
 * Get the current noise level of a wireless device.
 *
 */
guint8 nm_device_get_noise (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, 0);
	g_return_val_if_fail (nm_device_is_wireless (dev), 0);

	return (dev->options.wireless.noise);
}


/*
 * nm_device_get_max_quality
 *
 * Get the quality maximum of a wireless device.
 *
 */
guint8 nm_device_get_max_quality (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, 0);
	g_return_val_if_fail (nm_device_is_wireless (dev), 0);

	return (dev->options.wireless.max_quality);
}


/*
 * nm_device_get_bad_crypt_packets
 *
 * Return the number of packets the card has dropped because
 * they could not be successfully decrypted.
 *
 */
guint32 nm_device_get_bad_crypt_packets (NMDevice *dev)
{
	iwstats	stats;
	int		sk;
	int		err;

	g_return_val_if_fail (dev != NULL, 0);
	g_return_val_if_fail (nm_device_is_wireless (dev), 0);

	sk = iw_sockets_open ();
	err = iw_get_stats (sk, nm_device_get_iface (dev), &stats, NULL, FALSE);
	close (sk);
	return (err == 0 ? stats.discard.code : 0);
}


/*
 * nm_device_get_ip4_address
 *
 * Get a device's IPv4 address
 *
 */
guint32 nm_device_get_ip4_address(NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, 0);

	return (dev->ip4_address);
}

void nm_device_update_ip4_address (NMDevice *dev)
{
	guint32		new_address;
	struct ifreq	req;
	int			sk;
	int			err;
	
	g_return_if_fail (dev  != NULL);
	g_return_if_fail (dev->app_data != NULL);
	g_return_if_fail (nm_device_get_iface (dev) != NULL);

	/* Test devices get a nice, bogus IP address */
	if (dev->test_device)
	{
		dev->ip4_address = 0x07030703;
		return;
	}

	if ((sk = nm_device_open_sock ()) < 0)
		return;
	
	memset (&req, 0, sizeof (struct ifreq));
	strncpy ((char *)(&req.ifr_name), nm_device_get_iface (dev), strlen (nm_device_get_iface (dev)));
	err = ioctl (sk, SIOCGIFADDR, &req);
	close (sk);
	if (err != 0)
		return;

	new_address = ((struct sockaddr_in *)(&req.ifr_addr))->sin_addr.s_addr;

	/* If the new address is different, send an IP4AddressChanged signal on the bus */
	if (new_address != nm_device_get_ip4_address (dev))
	{
		nm_dbus_signal_device_ip4_address_change (dev->app_data->dbus_connection, dev);
		dev->ip4_address = new_address;
	}
}


/*
 * nm_device_get_ip6_address
 *
 * Get a device's IPv6 address
 *
 */
void nm_device_get_ip6_address(NMDevice *dev)
{
	/* FIXME
	 * Implement
	 */
}


/*
 * nm_device_get_hw_address
 *
 * Get a device's hardware address
 *
 */
void nm_device_get_hw_address(NMDevice *dev, unsigned char hw_addr[ETH_ALEN])
{
	g_return_if_fail (dev != NULL);

	memcpy (hw_addr, dev->hw_addr, ETH_ALEN);
}

void nm_device_update_hw_address (NMDevice *dev)
{
	struct ifreq	req;
	int			sk;
	int			err;

	g_return_if_fail (dev  != NULL);
	g_return_if_fail (dev->app_data != NULL);
	g_return_if_fail (nm_device_get_iface (dev) != NULL);

	/* Test devices get a nice, bogus IP address */
	if (dev->test_device)
	{
		memset (dev->hw_addr, 0, ETH_ALEN);
		return;
	}

	if ((sk = nm_device_open_sock ()) < 0)
		return;
	
	memset (&req, 0, sizeof (struct ifreq));
	strncpy ((char *)(&req.ifr_name), nm_device_get_iface (dev), strlen (nm_device_get_iface (dev)));
	err = ioctl (sk, SIOCGIFHWADDR, &req);
	close (sk);
	if (err != 0)
		return;

      memcpy (dev->hw_addr, req.ifr_hwaddr.sa_data, ETH_ALEN);
}


/*
 * nm_device_set_up_down
 *
 * Set the up flag on the device on or off
 *
 */
static void nm_device_set_up_down (NMDevice *dev, gboolean up)
{
	struct ifreq	ifr;
	int			sk;
	int			err;
	guint32		flags = up ? IFF_UP : ~IFF_UP;

	g_return_if_fail (dev != NULL);

	/* Test devices do whatever we tell them to do */
	if (dev->test_device)
	{
		dev->test_device_up = up;
		return;
	}

	if (nm_device_get_driver_support_level (dev) == NM_DRIVER_UNSUPPORTED)
		return;

	sk = nm_device_open_sock ();
	if (sk < 0)
		return;

	/* Get flags already there */
	strcpy (ifr.ifr_name, nm_device_get_iface (dev));
	err = ioctl (sk, SIOCGIFFLAGS, &ifr);
	if (!err)
	{
		/* If the interface doesn't have those flags already,
		 * set them on it.
		 */
		if ((ifr.ifr_flags^flags) & IFF_UP)
		{
			ifr.ifr_flags &= ~IFF_UP;
			ifr.ifr_flags |= IFF_UP & flags;
			if ((err = ioctl (sk, SIOCSIFFLAGS, &ifr)))
				syslog (LOG_ERR, "nm_device_set_up_down() could not bring device %s %s.  errno = %d", nm_device_get_iface (dev), (up ? "up" : "down"), errno );
		}
	}
	else
		syslog (LOG_ERR, "nm_device_set_up_down() could not get flags for device %s.  errno = %d", nm_device_get_iface (dev), errno );

	close (sk);
}


/*
 * Interface state functions: bring up, down, check
 *
 */
void nm_device_bring_up (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);

	nm_device_set_up_down (dev, TRUE);
}

void nm_device_bring_down (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);

	nm_device_set_up_down (dev, FALSE);
}

gboolean nm_device_is_up (NMDevice *dev)
{
	int			sk;
	struct ifreq	ifr;
	int			err;

	g_return_val_if_fail (dev != NULL, FALSE);

	if (dev->test_device)
		return (dev->test_device_up);

	sk = nm_device_open_sock ();
	if (sk < 0)
		return (FALSE);

	/* Get device's flags */
	strcpy (ifr.ifr_name, nm_device_get_iface (dev));
	err = ioctl (sk, SIOCGIFFLAGS, &ifr);
	close (sk);
	if (!err)
		return (!((ifr.ifr_flags^IFF_UP) & IFF_UP));

	syslog (LOG_ERR, "nm_device_is_up() could not get flags for device %s.  errno = %d", nm_device_get_iface (dev), errno );
	return (FALSE);
}


/*
 * nm_device_get_mode
 *
 * Get managed/infrastructure/adhoc mode on a device (currently wireless only)
 *
 */
NMNetworkMode nm_device_get_mode (NMDevice *dev)
{
	int			sk;
	NMNetworkMode	mode = NETWORK_MODE_UNKNOWN;

	g_return_val_if_fail (dev != NULL, NETWORK_MODE_UNKNOWN);
	g_return_val_if_fail (nm_device_is_wireless (dev), NETWORK_MODE_UNKNOWN);

	/* Force the card into Managed/Infrastructure mode */
	sk = iw_sockets_open ();
	if (sk >= 0)
	{
		struct iwreq	wrq;
		int			err;

		err = iw_set_ext (sk, nm_device_get_iface (dev), SIOCGIWMODE, &wrq);
		if (err == 0)
		{
			switch (wrq.u.mode)
			{
				case IW_MODE_INFRA:
					mode = NETWORK_MODE_INFRA;
					break;
				case IW_MODE_ADHOC:
					mode = NETWORK_MODE_ADHOC;
					break;
				default:
					break;
			}
		}
		else
			syslog (LOG_ERR, "nm_device_get_mode (%s): error setting card to Infrastructure mode.  errno = %d", nm_device_get_iface (dev), errno);				
		close (sk);
	}

	return (mode);
}


/*
 * nm_device_set_mode
 *
 * Set managed/infrastructure/adhoc mode on a device (currently wireless only)
 *
 */
gboolean nm_device_set_mode (NMDevice *dev, const NMNetworkMode mode)
{
	int			sk;
	gboolean		success = FALSE;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (nm_device_is_wireless (dev), FALSE);
	g_return_val_if_fail ((mode == NETWORK_MODE_INFRA) || (mode == NETWORK_MODE_ADHOC), FALSE);

	if (nm_device_get_mode (dev) == mode)
		return TRUE;

	/* Force the card into Managed/Infrastructure mode */
	sk = iw_sockets_open ();
	if (sk >= 0)
	{
		struct iwreq	wreq;
		int			err;
		gboolean		mode_good = FALSE;

		switch (mode)
		{
			case NETWORK_MODE_INFRA:
				wreq.u.mode = IW_MODE_INFRA;
				mode_good = TRUE;
				break;
			case NETWORK_MODE_ADHOC:
				wreq.u.mode = IW_MODE_ADHOC;
				mode_good = TRUE;
				break;
			default:
				mode_good = FALSE;
				break;
		}
		if (mode_good)
		{
			err = iw_set_ext (sk, nm_device_get_iface (dev), SIOCSIWMODE, &wreq);
			if (err == 0)
				success = TRUE;
			else
				syslog (LOG_ERR, "nm_device_set_mode (%s): error setting card to Infrastructure mode.  errno = %d", nm_device_get_iface (dev), errno);				
		}
		close (sk);
	}

	return (success);
}


/*
 * nm_device_activation_schedule_finish
 *
 * Schedule an idle routine in the main thread to finish the activation.
 *
 */
void nm_device_activation_schedule_finish (NMDevice *dev, DeviceStatus activation_result)
{
	GSource			*source = NULL;
	guint			 source_id = 0;
	NMActivationResult	*result = NULL;

	g_return_if_fail (dev != NULL);
	g_return_if_fail (dev->app_data != NULL);

	result = g_malloc0 (sizeof (NMActivationResult));
	nm_device_ref (dev);	/* Ref device for idle handler */
	result->dev = dev;
	result->result = activation_result;

	source = g_idle_source_new ();
	g_source_set_callback (source, nm_policy_activation_finish, (gpointer)result, NULL);
	g_source_attach (source, dev->app_data->main_context);
	g_source_unref (source);
}


/*
 * nm_device_activation_schedule_start
 *
 * Tell the device thread to begin activation.
 *
 * Returns:	TRUE on success activation beginning
 *			FALSE on error beginning activation (bad params, couldn't create thread)
 *
 */
gboolean nm_device_activation_schedule_start (NMDevice *dev)
{
	GError	*error = NULL;
	NMData	*data = NULL;
	GSource	*source = NULL;
	guint	 source_id = 0;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (!dev->activating, TRUE);	/* Return if activation has already begun */

	data = dev->app_data;
	g_return_val_if_fail (data != NULL, FALSE);

	/* Reset communication flags between worker and main thread */
	dev->activating = TRUE;
	dev->quit_activation = FALSE;
	if (nm_device_is_wireless (dev))
	{
		nm_device_set_now_scanning (dev, TRUE);
		dev->options.wireless.user_key_received = FALSE;
	}

	if (nm_device_get_driver_support_level (dev) == NM_DRIVER_UNSUPPORTED)
	{
		dev->activating = FALSE;
		return (FALSE);
	}

	source = g_idle_source_new ();
	g_source_set_callback (source, nm_device_activate, dev, NULL);
	g_source_attach (source, dev->context);
	g_source_unref (source);

	nm_dbus_signal_device_status_change (data->dbus_connection, dev, DEVICE_ACTIVATING);

	return (TRUE);
}


/*
 * nm_device_activation_handle_cancel
 *
 * Check whether we should stop activation, and if so clean up flags
 * and other random things.
 *
 */
static gboolean nm_device_activation_handle_cancel (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, TRUE);

	/* If we were told to quit activation, stop the thread and return */
	if (dev->quit_activation)
	{
		syslog (LOG_DEBUG, "nm_device_activation_worker(%s): activation canceled.", nm_device_get_iface (dev));
		if (nm_device_is_wireless (dev))
			nm_device_set_now_scanning (dev, FALSE);
		return (TRUE);
	}

	return (FALSE);
}


/*
 * nm_device_wireless_wait_for_link
 *
 * Try to be clever about when the wireless card really has associated with the access point.
 * Return TRUE when we think that it has, and FALSE when we thing it has not associated.
 *
 */
static gboolean nm_device_wireless_wait_for_link (NMDevice *dev, const char *essid)
{
	struct timeval	end_time;
	struct timeval	cur_time;
	gboolean		link = FALSE;
	double		last_freq = 0;
	guint		assoc_count = 0;
	gint			pause_value;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (time > 0, FALSE);

	pause_value = nm_device_get_association_pause_value (dev);
	if (pause_value < 1)
		return FALSE;

	gettimeofday (&end_time, NULL);
	end_time.tv_sec += pause_value;

	/* We more or less keep asking the driver for the frequency the card is on, and
	 * when the frequency has stabilized (the driver has to scan channels to find the AP,
	 * and when it finds the AP it stops scanning) and the MAC is valid, we think we
	 * have a link.
	 */
	gettimeofday (&cur_time, NULL);
	while (cur_time.tv_sec < end_time.tv_sec)
	{
		double	 cur_freq = nm_device_get_frequency (dev);
		gboolean	 assoc = nm_device_wireless_is_associated (dev);
		char		*cur_essid = nm_device_get_essid (dev);

		if ((cur_freq == last_freq) && assoc && !strcmp (essid, cur_essid))
			assoc_count++;
		else
			assoc_count = 0;
		last_freq = cur_freq;

		g_usleep (G_USEC_PER_SEC / 2);
		if (nm_device_activation_should_cancel (dev))
			break;

		gettimeofday (&cur_time, NULL);
		if ((cur_time.tv_sec >= end_time.tv_sec) && (cur_time.tv_usec >= end_time.tv_usec))
			break;

		/* Assume that if we've been associated this long, we might as well just stop. */
		if (assoc_count >= 9)
			break;
	}

	/* If we've had a reasonable association count, we say we have a link */
	if (assoc_count > 6)
		link = TRUE;

	return (link);
}


/*
 * nm_device_activate_wireless
 *
 * Bring up a wireless card with the essid and wep key of its "best" ap
 *
 * Returns:	TRUE on successful activation
 *			FALSE on unsuccessful activation (ie no best AP)
 *
 */
static gboolean nm_device_set_wireless_config (NMDevice *dev, NMAccessPoint *ap)
{
	NMDeviceAuthMethod	 auth;
	gboolean			 success = FALSE;
	const char		*essid = NULL;

	g_return_val_if_fail (dev  != NULL, FALSE);
	g_return_val_if_fail (nm_device_is_wireless (dev), FALSE);
	g_return_val_if_fail (ap != NULL, FALSE);
	g_return_val_if_fail (nm_ap_get_essid (ap) != NULL, FALSE);
	g_return_val_if_fail (nm_ap_get_auth_method (ap) != NM_DEVICE_AUTH_METHOD_UNKNOWN, FALSE);

	/* Force the card into Managed/Infrastructure mode */
	nm_device_bring_down (dev);
	g_usleep (G_USEC_PER_SEC * 2);
	nm_device_bring_up (dev);
	g_usleep (G_USEC_PER_SEC * 2);
	nm_device_set_mode (dev, NETWORK_MODE_INFRA);

	essid = nm_ap_get_essid (ap);
	auth = nm_ap_get_auth_method (ap);

	nm_device_set_mode (dev, nm_ap_get_mode (ap));
	nm_device_set_bitrate (dev, 0);

	if (nm_ap_get_user_created (ap) || (nm_ap_get_freq (ap) && (nm_ap_get_mode (ap) == NETWORK_MODE_ADHOC)))
		nm_device_set_frequency (dev, nm_ap_get_freq (ap));
	else
		nm_device_set_frequency (dev, 0);	/* auto */

	if (nm_ap_get_encrypted (ap) && nm_ap_is_enc_key_valid (ap))
	{
		char				*hashed_key = nm_ap_get_enc_key_hashed (ap);

		if (auth == NM_DEVICE_AUTH_METHOD_NONE)
		{
			nm_ap_set_auth_method (ap, NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM);
			syslog (LOG_ERR, "Activation (%s/wireless): AP '%s' said it was encrypted, but had "
					"'none' for authentication method.  Using Open System authentication method.",
					nm_device_get_iface (dev), nm_ap_get_essid (ap));
		}
		nm_device_set_enc_key (dev, hashed_key, auth);
		g_free (hashed_key);
	}
	else
		nm_device_set_enc_key (dev, NULL, NM_DEVICE_AUTH_METHOD_NONE);

	nm_device_set_essid (dev, essid);

	syslog (LOG_INFO, "Activation (%s/wireless): using essid '%s', with %s authentication.",
			nm_device_get_iface (dev), essid, (auth == NM_DEVICE_AUTH_METHOD_NONE) ? "no" :
				((auth == NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM) ? "Open System" :
				((auth == NM_DEVICE_AUTH_METHOD_SHARED_KEY) ? "Shared Key" : "unknown")));

	/* Bring the device up and pause to allow card to associate. */
	g_usleep (G_USEC_PER_SEC * 2);

	/* Some cards don't really work well in ad-hoc mode unless you explicitly set the bitrate
	 * on them. (Netgear WG511T/Atheros 5212 with madwifi drivers).  Until we can get rate information
	 * from scanned access points out of iwlib, clamp bitrate for these cards at 11Mbps.
	 */
	if ((nm_ap_get_mode (ap) == NETWORK_MODE_ADHOC) && (nm_device_get_bitrate (dev) <= 0))
		nm_device_set_bitrate (dev, 11000);	/* In Kbps */

	return (TRUE);
}


/*
 * nm_device_activate_wireless_adhoc
 *
 * Create an ad-hoc network (rather than associating with one).
 *
 */
static gboolean nm_device_activate_wireless_adhoc (NMDevice *dev, NMAccessPoint *ap)
{
	gboolean			 success = FALSE;
	NMDeviceAuthMethod	 auth = NM_DEVICE_AUTH_METHOD_NONE;
	NMAPListIter		*iter;
	NMAccessPoint		*tmp_ap;
	double			 card_freqs[IW_MAX_FREQUENCIES];
	int				 num_freqs = 0, i;
	double			 freq_to_use = 0;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (ap != NULL, FALSE);

	if (nm_ap_get_encrypted (ap))
		auth = NM_DEVICE_AUTH_METHOD_SHARED_KEY;

	/* Build our local list of frequencies to whittle down until we find a free one */
	memset (&card_freqs, 0, sizeof (card_freqs));
	num_freqs = MIN (dev->options.wireless.range_info.num_frequency, IW_MAX_FREQUENCIES);
	for (i = 0; i < num_freqs; i++)
		card_freqs[i] = iw_freq2float (&(dev->options.wireless.range_info.freq[i]));

	/* We need to find a clear wireless channel to use.  We will
	 * only use 802.11b channels for now.
	 */
	iter = nm_ap_list_iter_new (nm_device_ap_list_get (dev));
	while ((tmp_ap = nm_ap_list_iter_next (iter)))
	{
		double ap_freq = nm_ap_get_freq (tmp_ap);
		for (i = 0; i < num_freqs && ap_freq; i++)
		{
			if (card_freqs[i] == ap_freq)
				card_freqs[i] = 0;
		}
	}
	nm_ap_list_iter_free (iter);

	/* Ok, find the first non-zero freq in our table and use it.
	 * For now we only try to use a channel in the 802.11b channel
	 * space so that most everyone can see it.
	 */
	for (i = 0; i < num_freqs; i++)
	{
		int channel = iw_freq_to_channel (card_freqs[i], &(dev->options.wireless.range_info));
		if (card_freqs[i] && (channel > 0) && (channel < 15))
		{
			freq_to_use = card_freqs[i];
			break;
		}
	}

	/* Hmm, no free channels in 802.11b space.  Pick one more or less randomly */
	if (!freq_to_use)
	{
		double pfreq;
		int	channel = (int)(random () % 14);
		int	err;

		err = iw_channel_to_freq (channel, &pfreq, &(dev->options.wireless.range_info));
		if (err == channel)
			freq_to_use = pfreq;
	}

	if (freq_to_use)
	{
		nm_ap_set_freq (ap, freq_to_use);
	
		syslog (LOG_INFO, "Will create network '%s' with frequency %f.\n", nm_ap_get_essid (ap), nm_ap_get_freq (ap));
		if ((success = nm_device_set_wireless_config (dev, ap)))
			success = nm_device_activation_configure_ip (dev, TRUE);
	}

	return (success);
}


static gboolean AP_NEED_KEY (NMDevice *dev, NMAccessPoint *ap)
{
	char		*essid;
	gboolean	 need_key = FALSE;

	g_return_val_if_fail (ap != NULL, FALSE);

	essid = nm_ap_get_essid (ap);

	if (!nm_ap_get_encrypted (ap))
	{
		syslog (LOG_NOTICE, "Activation (%s/wireless): access point '%s' is unencrypted, no key needed.",
			nm_device_get_iface (dev), essid ? essid : "(null)");
	}
	else
	{
		if (nm_ap_is_enc_key_valid (ap))
		{
			syslog (LOG_NOTICE, "Activation (%s/wireless): access point '%s' is encrypted, and a key exists.  No new key needed.",
					nm_device_get_iface (dev), essid ? essid : "(null)");
		}
		else
		{
			syslog (LOG_NOTICE, "Activation (%s/wireless): access point '%s' is encrypted, but NO valid key exists.  New key needed.",
					nm_device_get_iface (dev), essid ? essid : "(null)");
			need_key = TRUE;
		}
	}

	return (need_key);
}


/*
 * get_initial_auth_method
 *
 * Ensure the auth method the AP reports is valid for its encryption mode.
 *
 */
static NMDeviceAuthMethod get_initial_auth_method (NMAccessPoint *ap)
{
	g_return_val_if_fail (ap != NULL, NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM);

	if (nm_ap_get_encrypted (ap))
	{
		NMDeviceAuthMethod	auth = nm_ap_get_auth_method (ap);

		if (    (auth == NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM)
			|| (auth == NM_DEVICE_AUTH_METHOD_SHARED_KEY))
			return (nm_ap_get_auth_method (ap));
		else
			return (NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM);
	}

	return (NM_DEVICE_AUTH_METHOD_NONE);
}


void invalidate_ap (NMDevice *dev, NMAccessPoint *ap)
{
	g_return_if_fail (dev != NULL);
	g_return_if_fail (dev->app_data != NULL);
	g_return_if_fail (ap != NULL);

	/* If its an AP the user forced, notify the user it failed. */
	/* FIXME: we dont' set ap's that are in our scan list as "artificial",
	 * so we won't be able to signal the user when a connection to on of them
	 * failed.
	 */
	if (nm_ap_get_artificial (ap))
		nm_dbus_schedule_network_not_found_signal (dev->app_data, nm_ap_get_essid (ap));	

	nm_ap_set_invalid (ap, TRUE);
	nm_ap_list_append_ap (dev->app_data->invalid_ap_list, ap);
	nm_ap_unref (ap);
	nm_device_update_best_ap (dev);
}


/*
 * nm_device_activate_wireless
 *
 * Activate a wireless ethernet device.  Locking could be confusing here, pay attention to it.
 * We grab the scan mutex because scanning requires us to set certain state on the card,
 * like mode, which could screw up device activation link state checks.
 *
 */
static gboolean nm_device_activate_wireless (NMDevice *dev)
{
	NMAccessPoint		*best_ap;
	gboolean			 success = FALSE;
	guint8			 attempt = 1;
	char				 last_essid [50] = "\0";
	gboolean			 need_key = FALSE;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (dev->app_data != NULL, FALSE);

	/* Grab the scan mutex, we don't want the scan thread to mess up our settings
	 * during activation and link detection.
	 */
	g_mutex_lock (dev->options.wireless.scan_mutex);

	if (!nm_device_is_up (dev))
		nm_device_bring_up (dev);
	g_usleep (G_USEC_PER_SEC);

get_ap:
	/* If we were told to quit activation, stop the thread and return */
	if (nm_device_activation_handle_cancel (dev))
		goto out;

	/* Get a valid "best" access point we should connect to.  We don't hold the scan
	 * lock here because this might take a while.
	 */
	g_mutex_unlock (dev->options.wireless.scan_mutex);
	while (!(best_ap = nm_device_get_best_ap (dev)))
	{
		nm_device_set_now_scanning (dev, TRUE);
		syslog (LOG_DEBUG, "Activation (%s/wireless): waiting for an access point.", nm_device_get_iface (dev));
		g_usleep (G_USEC_PER_SEC * 2);

		/* If we were told to quit activation, stop the thread and return */
		if (nm_device_activation_handle_cancel (dev))
		{
			/* Wierd as it may seem, we lock here to balance the unlock in "out:" */
			g_mutex_lock (dev->options.wireless.scan_mutex);
			goto out;
		}
	}

	/* Set ESSID early so that when we send out the DeviceStatusChanged signal below,
	 * we are able to respond correctly to queries for "getActiveNetwork" against
	 * our device.  nm_device_get_path_for_ap() uses the /card's/ AP, not the best_ap.
	 */
	nm_device_set_essid (dev, nm_ap_get_essid (best_ap));

	nm_device_set_now_scanning (dev, FALSE);
	g_mutex_lock (dev->options.wireless.scan_mutex);

	if (nm_ap_get_artificial (best_ap))
	{
		/* Some Cisco cards (340/350 PCMCIA) don't return non-broadcasting APs
		 * in their scan results, so we can't know beforehand whether or not the
		 * AP was encrypted.  So we have to update their encryption status on the fly.
		 */
		if (nm_ap_get_encrypted (best_ap) || nm_ap_is_enc_key_valid (best_ap))
		{
			nm_ap_set_encrypted (best_ap, TRUE);
			nm_ap_set_auth_method (best_ap, NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM);
		}
	}

	need_key = AP_NEED_KEY (dev, best_ap);

need_key:
	if (need_key)
	{
		char	*essid = nm_ap_get_essid (best_ap);
		if (strcmp (essid, last_essid) != 0)
			attempt = 1;
		strncpy (&last_essid[0], essid, 49);

		/* Don't hold the mutex while waiting for a key */
		g_mutex_unlock (dev->options.wireless.scan_mutex);

		/* Get a wireless key */
		dev->options.wireless.user_key_received = FALSE;
		nm_dbus_get_user_key_for_network (dev->app_data->dbus_connection, dev, best_ap, attempt);
		attempt++;
		need_key = FALSE;

		/* Wait for the key to come back */
		syslog (LOG_DEBUG, "Activation (%s/wireless): asking for user key.", nm_device_get_iface (dev));
		while (!dev->options.wireless.user_key_received && !dev->quit_activation)
			g_usleep (G_USEC_PER_SEC / 2);

		syslog (LOG_DEBUG, "Activation (%s/wireless): user key received.", nm_device_get_iface (dev));

		/* Done waiting, grab lock again */
		g_mutex_lock (dev->options.wireless.scan_mutex);

		/* User may have cancelled the key request, so we need to update our best AP again. */
		nm_ap_unref (best_ap);

		goto get_ap;
	}

	if (nm_ap_get_mode (best_ap) == NETWORK_MODE_ADHOC)
	{
		/* Only do auto-ip on Ad-Hoc connections for now.  We technically
		 * could do DHCP on them though.
		 */
		success = nm_device_activation_configure_ip (dev, TRUE);
		goto connect_done;
	}

try_connect:
	/* Initial authentication method */
	nm_ap_set_auth_method (best_ap, get_initial_auth_method (best_ap));

	while (success == FALSE)
	{
		int			 ip_success = FALSE;
		NMAccessPoint	*tmp_ap = NULL;
		gboolean		 link = FALSE;
		gboolean		 adhoc = (nm_ap_get_mode (best_ap) == NETWORK_MODE_ADHOC);

		/* If we were told to quit activation, stop the thread and return */
		if (nm_device_activation_handle_cancel (dev))
			goto out;

		nm_device_set_wireless_config (dev, best_ap);

		link = nm_device_wireless_wait_for_link (dev, nm_ap_get_essid (best_ap));

		/* If we were told to quit activation, stop the thread and return */
		if (nm_device_activation_handle_cancel (dev))
			goto out;

		if (!link)
		{
			if (nm_ap_get_auth_method (best_ap) == NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM)
			{
				syslog (LOG_DEBUG, "Activation (%s/wireless): no hardware link to '%s' in Open System mode, trying Shared Key.",
						nm_device_get_iface (dev), nm_ap_get_essid (best_ap) ? nm_ap_get_essid (best_ap) : "(none)");
				/* Back down to Shared Key mode */
				nm_ap_set_auth_method (best_ap, NM_DEVICE_AUTH_METHOD_SHARED_KEY);
				continue;
			}
			else if (nm_ap_get_auth_method (best_ap) == NM_DEVICE_AUTH_METHOD_SHARED_KEY)
			{
				/* Must be in Open System mode and it still didn't work, so
				 * we'll invalidate the current "best" ap and get another one */
				syslog (LOG_DEBUG, "Activation (%s/wireless): no hardware link to '%s' in Shared Key mode, trying another access point.",
						nm_device_get_iface (dev), nm_ap_get_essid (best_ap) ? nm_ap_get_essid (best_ap) : "(none)");
			}
			else
			{
				syslog (LOG_DEBUG, "Activation (%s/wireless): no hardware link to '%s' in non-encrypted mode.",
						nm_device_get_iface (dev), nm_ap_get_essid (best_ap) ? nm_ap_get_essid (best_ap) : "(none)");
			}

			/* All applicable modes failed, invalidate current best_ap and get a new one */
			invalidate_ap (dev, best_ap);
			goto get_ap;
		}

		/* For those broken cards that report successful hardware link even when WEP key is wrong,
		 * and also for Open System mode (where you cannot know WEP key is wrong ever), we try to
		 * do DHCP and if that fails, fall back to next auth mode and try again.
		 */
		if ((success = nm_device_activation_configure_ip (dev, adhoc)))
		{
			/* Cache the last known good auth method in both NetworkManagerInfo and our allowed AP list */
			nm_dbus_update_network_auth_method (dev->app_data->dbus_connection, nm_ap_get_essid (best_ap), nm_ap_get_auth_method (best_ap));
			if ((tmp_ap = nm_ap_list_get_ap_by_essid (dev->app_data->allowed_ap_list, nm_ap_get_essid (best_ap))))
				nm_ap_set_auth_method (tmp_ap, nm_ap_get_auth_method (best_ap));
		}
		else
		{
			/* If we were told to quit activation, stop the thread and return */
			if (nm_device_activation_handle_cancel (dev))
				goto out;

			if ((nm_ap_get_auth_method (best_ap) == NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM) && !adhoc)
			{
				/* Back down to Shared Key mode */
				syslog (LOG_DEBUG, "Activation (%s/wireless): could not get IP configuration info for '%s' in Open System mode, trying Shared Key.",
						nm_device_get_iface (dev), nm_ap_get_essid (best_ap) ? nm_ap_get_essid (best_ap) : "(none)");
				nm_ap_set_auth_method (best_ap, NM_DEVICE_AUTH_METHOD_SHARED_KEY);
				continue;
			}
			else if ((nm_ap_get_auth_method (best_ap) == NM_DEVICE_AUTH_METHOD_SHARED_KEY) && !adhoc)
			{
				/* Shared Key mode failed, we must have bad WEP key */
				syslog (LOG_DEBUG, "Activation (%s/wireless): could not get IP configuration info for '%s' in Shared Key mode, asking for new key.",
						nm_device_get_iface (dev), nm_ap_get_essid (best_ap) ? nm_ap_get_essid (best_ap) : "(none)");
				need_key = TRUE;
				goto need_key;
			}
			else
			{
				/* All applicable modes failed, invalidate current best_ap and get a new one */
				invalidate_ap (dev, best_ap);
				goto get_ap;
			}
		}
	}

connect_done:
	/* If we were told to quit activation, stop the thread and return */
	if (nm_device_activation_handle_cancel (dev))
		goto out;

	if (success)
	{
		syslog (LOG_DEBUG, "Activation (%s/wireless): Success!  Connected to access point '%s' and got an IP address.",
				nm_device_get_iface (dev), nm_ap_get_essid (best_ap) ? nm_ap_get_essid (best_ap) : "(none)");
		nm_ap_unref (best_ap);
	}

out:
	nm_device_set_now_scanning (dev, FALSE);
	g_mutex_unlock (dev->options.wireless.scan_mutex);
	return (success);
}


/*
 * nm_device_activation_configure_ip
 *
 * Perform any IP-based configuration on a device, like running DHCP
 * or manually setting up the IP address, gateway, and default route.
 *
 */
static gboolean nm_device_activation_configure_ip (NMDevice *dev, gboolean do_only_autoip)
{
	gboolean success = FALSE;

	g_return_val_if_fail (dev != NULL, FALSE);

	nm_system_delete_default_route ();
	if (do_only_autoip)
	{
		success = nm_device_do_autoip (dev);
	}
	else if (nm_device_config_get_use_dhcp (dev))
	{
		int		err;

		err = nm_device_dhcp_request (dev);
		if (err == RET_DHCP_BOUND)
			success = TRUE;
		else
		{
			/* Interfaces cannot be down if they are the active interface,
			 * otherwise we cannot use them for scanning or link detection.
			 */
			if (nm_device_is_wireless (dev))
			{
				nm_device_set_essid (dev, "");
				nm_device_set_enc_key (dev, NULL, NM_DEVICE_AUTH_METHOD_NONE);
			}

			if (!nm_device_is_up (dev))
				nm_device_bring_up (dev);
		}
	}
	else
	{
		/* Manually set up the device */
		success = nm_system_device_setup_static_ip4_config (dev);
	}

	if (success)
	{
		nm_system_flush_arp_cache ();
		nm_system_restart_mdns_responder ();
	}

	return (success);
}


/*
 * nm_device_activate
 *
 * Activate a device, done from the device's worker thread.
 *
 */
static gboolean nm_device_activate (gpointer user_data)
{
	NMDevice			*dev = (NMDevice *)user_data;
	gboolean			 success = FALSE;
	gboolean			 finished = FALSE;
	GMainContext		*context = NULL;

	g_return_val_if_fail (dev  != NULL, FALSE);
	g_return_val_if_fail (dev->app_data != NULL, FALSE);

	syslog (LOG_DEBUG, "Activation (%s) started...", nm_device_get_iface (dev));

	/* Bring the device up */
	if (!nm_device_is_up (dev));
		nm_device_bring_up (dev);

	if (nm_device_is_wireless (dev))
	{
		gboolean		create_network = FALSE;
		NMAccessPoint *best_ap = nm_device_get_best_ap (dev);

		if (best_ap)
		{
			if (nm_ap_get_user_created (best_ap))
			{
				create_network = TRUE;
				syslog (LOG_INFO, "Creating wireless network '%s'.\n", nm_ap_get_essid (best_ap));
				success = nm_device_activate_wireless_adhoc (dev, best_ap);
				syslog (LOG_INFO, "Wireless network creation for '%s' was %s.\n", nm_ap_get_essid (best_ap), success ? "successful" : "unsuccessful");
			}
			nm_ap_unref (best_ap);
		}

		if (!create_network)
			success = nm_device_activate_wireless (dev);
	}
	else if (nm_device_is_wired (dev))
		success = nm_device_activation_configure_ip (dev, FALSE);

	/* If we were told to quit activation, stop the thread and return */
	if (nm_device_activation_handle_cancel (dev))
		goto out;

	if (success)
		syslog (LOG_DEBUG, "Activation (%s) IP configuration/DHCP successful!\n", nm_device_get_iface (dev));
	else
		syslog (LOG_DEBUG, "Activation (%s) IP configuration/DHCP unsuccessful!  Ending activation...\n", nm_device_get_iface (dev));

	/* Setup DHCP timeouts if we need to renew/rebind at any point */
	if (nm_device_config_get_use_dhcp (dev) && dev->dhcp_iface)
		nm_device_dhcp_setup_timeouts (dev);

	finished = TRUE;

out:
	if (dev->dhcp_iface)
	{
		dhcp_interface_free (dev->dhcp_iface);
		dev->dhcp_iface = NULL;
	}

	syslog (LOG_DEBUG, "Activation (%s) ended.\n", nm_device_get_iface (dev));
	dev->activating = FALSE;
	dev->quit_activation = FALSE;
	if (finished)
		nm_device_activation_schedule_finish (dev, success ? DEVICE_NOW_ACTIVE : DEVICE_ACTIVATION_FAILED);

	return FALSE;
}


/*
 * nm_device_is_activating
 *
 * Return whether or not the device is currently activating itself.
 *
 */
gboolean nm_device_is_activating (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	return (dev->activating);
}


/*
 * nm_device_activation_should_cancel
 *
 * Return whether or not we've been told to cancel activation
 *
 */
gboolean nm_device_activation_should_cancel (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	return (dev->quit_activation);
}


/*
 * nm_device_activation_cancel
 *
 * Signal activation worker that it should stop and die.
 *
 */
void nm_device_activation_cancel (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);

	if (nm_device_is_activating (dev))
	{
		syslog (LOG_DEBUG, "nm_device_activation_cancel(%s): cancelling...", nm_device_get_iface (dev));
		dev->quit_activation = TRUE;

		/* Spin until cancelled.  Possible race conditions or deadlocks here.
		 * The other problem with waiting here is that we hold up dbus traffic
		 * that we should respond to.
		 */
		while (nm_device_is_activating (dev))
		{
			/* Nice race here between quit activation and dhcp.  We may not have
			 * started DHCP when we're told to quit activation, so we need to keep
			 * signalling dhcp to quit, which it will pick up whenever it starts.
			 * This should really be taken care of a better way.
			 */
			if (dev->dhcp_iface)
				nm_device_dhcp_cease (dev);

			g_usleep (G_USEC_PER_SEC / 2);
		}
		syslog (LOG_DEBUG, "nm_device_activation_cancel(%s): cancelled.", nm_device_get_iface (dev));
	}
}


/*
 * nm_device_deactivate
 *
 * Remove a device's routing table entries and IP address.
 *
 */
gboolean nm_device_deactivate (NMDevice *dev, gboolean just_added)
{
	g_return_val_if_fail (dev  != NULL, FALSE);
	g_return_val_if_fail (dev->app_data != NULL, FALSE);

	nm_device_activation_cancel (dev);

	if (nm_device_get_driver_support_level (dev) == NM_DRIVER_UNSUPPORTED)
		return (TRUE);

	/* Take out any entries in the routing table and any IP address the device had. */
	nm_system_device_flush_routes (dev);
	nm_system_device_flush_addresses (dev);
	dev->ip4_address = 0;

	if (!just_added && (dev == dev->app_data->active_device))
		nm_dbus_signal_device_status_change (dev->app_data->dbus_connection, dev, DEVICE_NO_LONGER_ACTIVE);

	/* Clean up stuff, don't leave the card associated */
	if (nm_device_is_wireless (dev))
	{
		nm_device_set_essid (dev, "");
		nm_device_set_enc_key (dev, NULL, NM_DEVICE_AUTH_METHOD_NONE);
		nm_device_set_mode (dev, NETWORK_MODE_INFRA);
		dev->options.wireless.scan_interval = 20;
	}

	return (TRUE);
}


/*
 * nm_device_set_user_key_for_network
 *
 * Called upon receipt of a NetworkManagerInfo reply with a
 * user-supplied key.
 *
 */
void nm_device_set_user_key_for_network (NMDevice *dev, NMAccessPointList *invalid_list,
									unsigned char *network, unsigned char *key,
									NMEncKeyType enc_type)
{
	NMAccessPoint	*best_ap;
	const char 	*cancel_message = "***canceled***";

	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));
	g_return_if_fail (network != NULL);
	g_return_if_fail (key != NULL);

	/* If the user canceled, mark the ap as invalid */
	if (strncmp (key, cancel_message, strlen (cancel_message)) == 0)
	{
		NMAccessPoint	*ap;

		if ((ap = nm_device_ap_list_get_ap_by_essid (dev, network)))
		{
			NMAccessPoint	*invalid_ap = nm_ap_new_from_ap (ap);
			if (invalid_list)
				nm_ap_list_append_ap (invalid_list, invalid_ap);
		}

		nm_device_update_best_ap (dev);
	}
	else if ((best_ap = nm_device_get_best_ap (dev)))
	{
		/* Make sure the "best" ap matches the essid we asked for the key of,
		 * then set the new key on the access point.
		 */
		if (nm_null_safe_strcmp (network, nm_ap_get_essid (best_ap)) == 0)
			nm_ap_set_enc_key_source (best_ap, key, enc_type);

		nm_ap_unref (best_ap);
	}
	dev->options.wireless.user_key_received = TRUE;
}


/*
 * nm_device_ap_list_add_ap
 *
 * Add an access point to the devices internal AP list.
 *
 */
static void nm_device_ap_list_add_ap (NMDevice *dev, NMAccessPoint *ap)
{
	g_return_if_fail (dev != NULL);
	g_return_if_fail (ap  != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	nm_ap_list_append_ap (dev->options.wireless.ap_list, ap);
	/* Transfer ownership of ap to the list by unrefing it here */
	nm_ap_unref (ap);
}


/*
 * nm_device_ap_list_clear
 *
 * Clears out the device's internal list of available access points.
 *
 */
void	nm_device_ap_list_clear (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	if (!dev->options.wireless.ap_list)
		return;

	nm_ap_list_unref (dev->options.wireless.ap_list);
	dev->options.wireless.ap_list = NULL;
}


/*
 * nm_device_ap_list_get_ap_by_essid
 *
 * Get the access point for a specific essid
 *
 */
NMAccessPoint *nm_device_ap_list_get_ap_by_essid (NMDevice *dev, const char *essid)
{
	NMAccessPoint	*ret_ap = NULL;

	g_return_val_if_fail (dev != NULL, NULL);
	g_return_val_if_fail (nm_device_is_wireless (dev), NULL);
	g_return_val_if_fail (essid != NULL, NULL);

	if (!dev->options.wireless.ap_list)
		return (NULL);

	ret_ap = nm_ap_list_get_ap_by_essid (dev->options.wireless.ap_list, essid);

	return (ret_ap);
}


/*
 * nm_device_ap_list_get_ap_by_address
 *
 * Get the access point for a specific MAC address
 *
 */
NMAccessPoint *nm_device_ap_list_get_ap_by_address (NMDevice *dev, const struct ether_addr *addr)
{
	NMAccessPoint	*ret_ap = NULL;

	g_return_val_if_fail (dev != NULL, NULL);
	g_return_val_if_fail (nm_device_is_wireless (dev), NULL);
	g_return_val_if_fail (addr != NULL, NULL);

	if (!dev->options.wireless.ap_list)
		return (NULL);

	ret_ap = nm_ap_list_get_ap_by_address (dev->options.wireless.ap_list, addr);

	return (ret_ap);
}


/*
 * nm_device_ap_list_get
 *
 * Return a pointer to the AP list
 *
 */
NMAccessPointList *nm_device_ap_list_get (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, NULL);
	g_return_val_if_fail (nm_device_is_wireless (dev), NULL);

	return (dev->options.wireless.ap_list);
}

/*
 * Get/Set functions for "best" access point
 *
 * Caller MUST unref returned access point when done with it.
 *
 */
NMAccessPoint *nm_device_get_best_ap (NMDevice *dev)
{
	NMAccessPoint	*best_ap;

	g_return_val_if_fail (dev != NULL, NULL);
	g_return_val_if_fail (nm_device_is_wireless (dev), NULL);

	g_mutex_lock (dev->options.wireless.best_ap_mutex);
	best_ap = dev->options.wireless.best_ap;
	/* Callers get a reffed AP */
	if (best_ap) nm_ap_ref (best_ap);
	g_mutex_unlock (dev->options.wireless.best_ap_mutex);
	
	return (best_ap);
}

void nm_device_set_best_ap (NMDevice *dev, NMAccessPoint *ap)
{
	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	g_mutex_lock (dev->options.wireless.best_ap_mutex);

	if (dev->options.wireless.best_ap)
		nm_ap_unref (dev->options.wireless.best_ap);

	if (ap)
		nm_ap_ref (ap);

	dev->options.wireless.best_ap = ap;
	nm_device_unfreeze_best_ap (dev);
	g_mutex_unlock (dev->options.wireless.best_ap_mutex);
}


/*
 * Freeze/unfreeze best ap
 *
 * If the user explicitly picks a network to associate with, we don't
 * change the active network until it goes out of range.
 *
 */
void nm_device_freeze_best_ap (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	dev->options.wireless.freeze_best_ap = TRUE;
}

void nm_device_unfreeze_best_ap (NMDevice *dev)
{
	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	dev->options.wireless.freeze_best_ap = FALSE;
}

gboolean nm_device_is_best_ap_frozen (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (nm_device_is_wireless (dev), FALSE);

	return (dev->options.wireless.freeze_best_ap);
}


/*
 * Accessor for dhcp_interface
 *
 */
struct dhcp_interface *nm_device_get_dhcp_iface (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	return (dev->dhcp_iface);
}

void nm_device_set_dhcp_iface (NMDevice *dev, struct dhcp_interface *dhcp_iface)
{
	g_return_if_fail (dev != NULL);

	/* NOTE: this function should only be used from the activation worker thread
	 * which will take care of shutting down any active DHCP threads and cleaning
	 * up the dev->dhcp_iface structure.
	 */

	dev->dhcp_iface = dhcp_iface;
}


/*
 * nm_device_get_path_for_ap
 *
 * Return the object path for an access point.
 *
 * NOTE: assumes the access point is actually in the device's access point list.
 *
 */
char * nm_device_get_path_for_ap (NMDevice *dev, NMAccessPoint *ap)
{
	g_return_val_if_fail (dev != NULL, NULL);
	g_return_val_if_fail (ap  != NULL, NULL);

	if (nm_ap_get_essid (ap))
		return (g_strdup_printf ("%s/%s/Networks/%s", NM_DBUS_PATH_DEVICES, nm_device_get_iface (dev), nm_ap_get_essid (ap)));
	else
		return (NULL);
}


/*
 * nm_device_need_ap_switch
 *
 * Returns TRUE if the essid of the card does not match the essid
 * of the "best" access point it should be associating with.
 *
 */
gboolean nm_device_need_ap_switch (NMDevice *dev)
{
	NMAccessPoint	*ap;
	gboolean		 need_switch = FALSE;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (nm_device_is_wireless (dev), FALSE);

	ap = nm_device_get_best_ap (dev);
	if (nm_null_safe_strcmp (nm_device_get_essid (dev), (ap ? nm_ap_get_essid (ap) : NULL)) != 0)
		need_switch = TRUE;

	if (ap) nm_ap_unref (ap);

	return (need_switch);
}


/*
 * nm_device_update_best_ap
 *
 * Recalculate the "best" access point we should be associating with.  This
 * function may disrupt the current connection, so it should be called only
 * when necessary, ie when the current access point is no longer in range
 * or is for some other reason invalid and should no longer be used.
 *
 */
void nm_device_update_best_ap (NMDevice *dev)
{
	NMAccessPointList	*ap_list;
	NMAPListIter		*iter;
	NMAccessPoint		*scan_ap = NULL;
	NMAccessPoint		*best_ap = NULL;
	NMAccessPoint		*trusted_best_ap = NULL;
	NMAccessPoint		*untrusted_best_ap = NULL;
	GTimeVal			 trusted_latest_timestamp = {0, 0};
	GTimeVal			 untrusted_latest_timestamp = {0, 0};

	g_return_if_fail (dev != NULL);
	g_return_if_fail (dev->app_data != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	if (!(ap_list = nm_device_ap_list_get (dev)))
		return;

	/* Iterate over the device's ap list to make sure the current
	 * "best" ap is still in the device's ap list (so that if its
	 * not, we can "unfreeze" the best ap if its been frozen already).
	 * If it is, we don't change the best ap here.
	 */
	if (nm_device_is_best_ap_frozen (dev))
	{
		best_ap = nm_device_get_best_ap (dev);

		/* If its in the device's ap list still, don't change the
		 * best ap, since its frozen.
		 */
		g_mutex_lock (dev->options.wireless.best_ap_mutex);
		if (best_ap)
		{
			char *essid = nm_ap_get_essid (best_ap);
			/* Two reasons to keep the current best_ap:
			 * 1) Its still valid and we see it in our scan data
			 * 2) Its an ad-hoc network that we've created (and therefore its not in our scan data)
			 */
			if (    (    !nm_ap_list_get_ap_by_essid (dev->app_data->invalid_ap_list, essid)
					&& nm_device_ap_list_get_ap_by_essid (dev, essid))
				|| nm_ap_get_user_created (best_ap))
			{
				nm_ap_unref (best_ap);
				g_mutex_unlock (dev->options.wireless.best_ap_mutex);
				return;
			}
			nm_ap_unref (best_ap);
		}

		/* Otherwise, its gone away and we don't care about it anymore */
		nm_device_unfreeze_best_ap (dev);
		g_mutex_unlock (dev->options.wireless.best_ap_mutex);
	}

	if (!(iter = nm_ap_list_iter_new (ap_list)))
		return;
	while ((scan_ap = nm_ap_list_iter_next (iter)))
	{
		NMAccessPoint	*tmp_ap;
		char			*ap_essid = nm_ap_get_essid (scan_ap);

		/* Access points in the "invalid" list cannot be used */
		if (nm_ap_list_get_ap_by_essid (dev->app_data->invalid_ap_list, ap_essid))
			continue;

		if ((tmp_ap = nm_ap_list_get_ap_by_essid (dev->app_data->allowed_ap_list, ap_essid)))
		{
			const GTimeVal *curtime = nm_ap_get_timestamp (tmp_ap);

			if (nm_ap_get_trusted (tmp_ap) && (curtime->tv_sec > trusted_latest_timestamp.tv_sec))
			{
				trusted_latest_timestamp = *nm_ap_get_timestamp (tmp_ap);
				trusted_best_ap = scan_ap;
				/* Merge access point data (mainly to get updated WEP key) */
				nm_ap_set_enc_key_source (trusted_best_ap, nm_ap_get_enc_key_source (tmp_ap), nm_ap_get_enc_type (tmp_ap));
			}
			else if (!nm_ap_get_trusted (tmp_ap) && (curtime->tv_sec > untrusted_latest_timestamp.tv_sec))
			{
				untrusted_latest_timestamp = *nm_ap_get_timestamp (tmp_ap);
				untrusted_best_ap = scan_ap;
				/* Merge access point data (mainly to get updated WEP key) */
				nm_ap_set_enc_key_source (untrusted_best_ap, nm_ap_get_enc_key_source (tmp_ap), nm_ap_get_enc_type (tmp_ap));
			}
		}
	}
	best_ap = trusted_best_ap ? trusted_best_ap : untrusted_best_ap;
	nm_ap_list_iter_free (iter);

	/* If the best ap is NULL, bring device down and clear out its essid and AP */
	nm_device_set_best_ap (dev, best_ap);
}


typedef struct NMDeviceForceData
{
	NMDevice		*dev;
	const char	*net;
	const char	*key;
	NMEncKeyType	 key_type;
} NMDeviceForceData;


static gboolean nm_device_wireless_force_use (NMDevice *dev, const char *essid, const char *key, NMEncKeyType key_type)
{
	gboolean			 encrypted = FALSE;
	NMAccessPoint		*ap = NULL;
	NMAccessPoint		*tmp_ap = NULL;
	gboolean			 success = FALSE;
	gboolean			 exists = FALSE;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (dev->app_data != NULL, FALSE);

	if (!essid)
		return FALSE;

	syslog (LOG_DEBUG, "Forcing AP '%s'", essid);

	if (    key
		&& strlen (key)
		&& (key_type != NM_ENC_TYPE_UNKNOWN)
		&& (key_type != NM_ENC_TYPE_NONE))
		encrypted = TRUE;

	/* Find the AP in our card's scan list first.
	 * If its not there, create an entirely new AP.
	 */
	if (!(ap = nm_ap_list_get_ap_by_essid (nm_device_ap_list_get (dev), essid)))
	{
		/* Okay, the card didn't see it in the scan, Cisco cards sometimes do this.
		 * So we make a "fake" access point and add it to the scan list.
		 */
		ap = nm_ap_new ();
		nm_ap_set_essid (ap, essid);
		nm_ap_set_encrypted (ap, encrypted);		
		if (encrypted)
			nm_ap_set_auth_method (ap, NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM);
		else
			nm_ap_set_auth_method (ap, NM_DEVICE_AUTH_METHOD_NONE);
		nm_ap_set_artificial (ap, TRUE);
		nm_ap_list_append_ap (nm_device_ap_list_get (dev), ap);
		nm_ap_unref (ap);
	}

	/* Now that this AP has an essid, copy over encryption keys and whatnot */
	if ((tmp_ap = nm_ap_list_get_ap_by_essid (dev->app_data->allowed_ap_list, nm_ap_get_essid (ap))))
	{
		nm_ap_set_enc_key_source (ap, nm_ap_get_enc_key_source (tmp_ap), nm_ap_get_enc_type (tmp_ap));
		nm_ap_set_auth_method (ap, nm_ap_get_auth_method (tmp_ap));
		nm_ap_set_invalid (ap, nm_ap_get_invalid (tmp_ap));
		nm_ap_set_timestamp (ap, nm_ap_get_timestamp (tmp_ap));
	}

	/* Use the encryption key and type the user sent us if its valid */
	if (encrypted)
		nm_ap_set_enc_key_source (ap, key, key_type);

	nm_device_set_best_ap (dev, ap);
	nm_device_freeze_best_ap (dev);

	return TRUE;
}


gboolean nm_device_wired_force_use (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (dev->app_data != NULL, FALSE);

	return TRUE;
}


gboolean nm_device_force_use (gpointer user_data)
{
	NMDeviceForceData	*cb_data = (NMDeviceForceData *)user_data;
	NMData			*app_data = NULL;
	gboolean			 success = FALSE;

	g_return_val_if_fail (cb_data != NULL, FALSE);

	if (!cb_data->dev || !cb_data->dev->app_data)
		goto out;
	
	app_data = cb_data->dev->app_data;
	if (nm_device_is_wireless (cb_data->dev))
		success = nm_device_wireless_force_use (cb_data->dev, cb_data->net, cb_data->key, cb_data->key_type);
	else if (nm_device_is_wired (cb_data->dev))
		success = nm_device_wired_force_use (cb_data->dev);

	if (success)
		nm_policy_schedule_device_switch (cb_data->dev, cb_data->dev->app_data);
	
out:
	/* Function that scheduled us must ref the device */
	nm_device_unref (cb_data->dev);

	app_data->forcing_device = FALSE;
	g_free (cb_data);
	return FALSE;
}


void nm_device_schedule_force_use (NMDevice *dev, const char *network, const char *key, NMEncKeyType key_type)
{
	NMDeviceForceData	*cb_data;
	GSource			*source;

	g_return_if_fail (dev != NULL);
	g_return_if_fail (dev->app_data != NULL);
	g_return_if_fail (dev->app_data->main_context != NULL);

	cb_data = g_malloc0 (sizeof (NMDeviceForceData));
	cb_data->dev = dev;
	cb_data->net = network ? g_strdup (network) : NULL;
	cb_data->key = key ? g_strdup (key) : NULL;
	cb_data->key_type = key_type;

	source = g_idle_source_new ();
	g_source_set_callback (source, nm_device_force_use, cb_data, NULL);
	g_source_attach (source, dev->context);
	g_source_unref (source);
}


/*
 * nm_device_do_pseudo_scan
 *
 * Brute-force the allowed access point list to find one that works, if any.
 *
 * FIXME
 * There's probably a better way to do the non-scanning access point discovery
 * than brute forcing it like this, but that makes the state machine here oh so
 * much more complicated.
 */
static void nm_device_do_pseudo_scan (NMDevice *dev)
{
	NMAPListIter		*iter;
	NMAccessPoint		*ap;

	g_return_if_fail (dev  != NULL);
	g_return_if_fail (dev->app_data != NULL);

	/* Test devices shouldn't get here since we fake the AP list earlier */
	g_return_if_fail (!dev->test_device);

	nm_device_ref (dev);

	if (!(iter = nm_ap_list_iter_new (dev->app_data->allowed_ap_list)))
		return;

	nm_device_set_essid (dev, "");
	while ((ap = nm_ap_list_iter_next (iter)))
	{
		gboolean			valid = FALSE;
		struct ether_addr	save_ap_addr;
		struct ether_addr	cur_ap_addr;

		if (!nm_device_is_up (dev));
			nm_device_bring_up (dev);

		/* Save the MAC address */
		nm_device_get_ap_address (dev, &save_ap_addr);

		if (nm_ap_get_enc_key_source (ap))
		{
			char *hashed_key = nm_ap_get_enc_key_hashed (ap);
			nm_device_set_enc_key (dev, hashed_key, NM_DEVICE_AUTH_METHOD_SHARED_KEY);
			g_free (hashed_key);
		}
		else
			nm_device_set_enc_key (dev, NULL, NM_DEVICE_AUTH_METHOD_NONE);
		nm_device_set_essid (dev, nm_ap_get_essid (ap));

		/* Wait a bit for association */
		g_usleep (G_USEC_PER_SEC * nm_device_get_association_pause_value (dev));

		/* Do we have a valid MAC address? */
		nm_device_get_ap_address (dev, &cur_ap_addr);
		valid = nm_ethernet_address_is_valid (&cur_ap_addr);

		/* If the ap address we had before, and the ap address we
		 * have now, are the same, AP is invalid.  Certain cards (orinoco)
		 * will let the essid change, but the the card won't actually de-associate
		 * from the previous access point if it can't associate with the new one
		 * (ie signal too weak, etc).
		 */
		if (valid && (memcmp (&save_ap_addr, &cur_ap_addr, sizeof (struct ether_addr)) == 0))
			valid = FALSE;

		if (valid)
		{
			syslog(LOG_INFO, "%s: setting AP '%s' best", nm_device_get_iface (dev), nm_ap_get_essid (ap));

			nm_device_set_best_ap (dev, ap);
			nm_policy_schedule_state_update (dev->app_data);
			break;
		}
	}

	nm_ap_list_iter_free (iter);
	nm_device_unref (dev);
}


/*
 * nm_device_fake_ap_list
 *
 * Fake the access point list, used for test devices.
 *
 */
static void nm_device_fake_ap_list (NMDevice *dev)
{
	#define NUM_FAKE_APS	4

	int				 i;
	NMAccessPointList	*old_ap_list = nm_device_ap_list_get (dev);

	char				*fake_essids[NUM_FAKE_APS] = { "green", "bay", "packers", "rule" };
	struct ether_addr	 fake_addrs[NUM_FAKE_APS] =  {{{0x70, 0x37, 0x03, 0x70, 0x37, 0x03}},
											{{0x12, 0x34, 0x56, 0x78, 0x90, 0xab}},
											{{0xcd, 0xef, 0x12, 0x34, 0x56, 0x78}},
											{{0x90, 0xab, 0xcd, 0xef, 0x12, 0x34}} };
	guint8			 fake_qualities[NUM_FAKE_APS] = { 150, 26, 200, 100 };
	double			 fake_freqs[NUM_FAKE_APS] = { 3.1416, 4.1416, 5.1415, 6.1415 };
	gboolean			 fake_enc[NUM_FAKE_APS] = { FALSE, TRUE, FALSE, TRUE };

	g_return_if_fail (dev != NULL);
	g_return_if_fail (dev->app_data != NULL);

	dev->options.wireless.ap_list = nm_ap_list_new (NETWORK_TYPE_DEVICE);

	for (i = 0; i < NUM_FAKE_APS; i++)
	{
		NMAccessPoint		*nm_ap  = nm_ap_new ();
		NMAccessPoint		*list_ap;

		/* Copy over info from scan to local structure */
		nm_ap_set_essid (nm_ap, fake_essids[i]);

		if (fake_enc[i])
			nm_ap_set_encrypted (nm_ap, FALSE);
		else
			nm_ap_set_encrypted (nm_ap, TRUE);

		nm_ap_set_address (nm_ap, (const struct ether_addr *)(&fake_addrs[i]));
		nm_ap_set_strength (nm_ap, fake_qualities[i]);
		nm_ap_set_freq (nm_ap, fake_freqs[i]);

		/* Merge settings from wireless networks, mainly keys */
		if ((list_ap = nm_ap_list_get_ap_by_essid (dev->app_data->allowed_ap_list, nm_ap_get_essid (nm_ap))))
		{
			nm_ap_set_timestamp (nm_ap, nm_ap_get_timestamp (list_ap));
			nm_ap_set_enc_key_source (nm_ap, nm_ap_get_enc_key_source (list_ap), nm_ap_get_enc_type (list_ap));
		}

		/* Add the AP to the device's AP list */
		nm_device_ap_list_add_ap (dev, nm_ap);
	}

	if (dev == dev->app_data->active_device)
		nm_ap_list_diff (dev->app_data, dev, old_ap_list, nm_device_ap_list_get (dev));
	if (old_ap_list)
		nm_ap_list_unref (old_ap_list);
}


/*
 * nm_device_wireless_schedule_scan
 *
 * Schedule a wireless scan in the /device's/ thread.
 *
 */
static void nm_device_wireless_schedule_scan (NMDevice *dev)
{
	GSource	*wscan_source;
	guint	 wscan_source_id;

	g_return_if_fail (dev != NULL);
	g_return_if_fail (nm_device_is_wireless (dev));

	wscan_source = g_timeout_source_new (dev->options.wireless.scan_interval * 1000);
	g_source_set_callback (wscan_source, nm_device_wireless_scan, dev, NULL);
	wscan_source_id = g_source_attach (wscan_source, dev->context);
	g_source_unref (wscan_source);
}


/*
 * nm_device_wireless_process_scan_results
 *
 * Process results of an iwscan() into our own AP lists.  We're an idle function,
 * but we never reschedule ourselves.
 *
 */
static gboolean nm_device_wireless_process_scan_results (gpointer user_data)
{
	NMWirelessScanResults	*results = (NMWirelessScanResults *)user_data;
	NMDevice				*dev;
	wireless_scan			*tmp_ap;
	gboolean				 have_blank_essids = FALSE;
	NMAPListIter			*iter;
	GTimeVal				 cur_time;
	gboolean				 list_changed = FALSE;

	g_return_val_if_fail (results != NULL, FALSE);	

	dev = results->dev;

	if (!dev || !results->scan_head.result)
		return FALSE;

	/* Test devices get their info faked */
	if (dev->test_device)
	{
		nm_device_fake_ap_list (dev);
		return FALSE;
	}

	/* Devices that don't support scanning have their pseudo-scanning done in
	 * the main thread anyway.
	 */
	if (!nm_device_supports_wireless_scan (dev))
	{
		nm_device_do_pseudo_scan (dev);
		return FALSE;
	}

	/* Translate iwlib scan results to NM access point list */
	tmp_ap = results->scan_head.result;
	while (tmp_ap)
	{
		/* We need at least an ESSID or a MAC address for each access point */
		if (tmp_ap->b.has_essid || tmp_ap->has_ap_addr)
		{
			NMAccessPoint		*nm_ap  = nm_ap_new ();

			/* Copy over info from scan to local structure */

			/* ipw2x00 drivers fill in an essid of "<hidden>" if they think the access point
			 * is hiding its MAC address.  Sigh.
			 */
			if (    !tmp_ap->b.has_essid
				|| (tmp_ap->b.essid && !strlen (tmp_ap->b.essid))
				|| (tmp_ap->b.essid && !strcmp (tmp_ap->b.essid, "<hidden>")))
			{
				nm_ap_set_essid (nm_ap, NULL);
				have_blank_essids = TRUE;
			}
			else
				nm_ap_set_essid (nm_ap, tmp_ap->b.essid);

			if (tmp_ap->b.has_key && (tmp_ap->b.key_flags & IW_ENCODE_DISABLED))
			{
				nm_ap_set_encrypted (nm_ap, FALSE);
				nm_ap_set_auth_method (nm_ap, NM_DEVICE_AUTH_METHOD_NONE);
			}
			else
			{
				nm_ap_set_encrypted (nm_ap, TRUE);
				nm_ap_set_auth_method (nm_ap, NM_DEVICE_AUTH_METHOD_OPEN_SYSTEM);
			}

			if (tmp_ap->has_ap_addr)
				nm_ap_set_address (nm_ap, (const struct ether_addr *)(tmp_ap->ap_addr.sa_data));

			if (tmp_ap->b.has_mode)
			{
				NMNetworkMode mode = NETWORK_MODE_INFRA;
				switch (tmp_ap->b.mode)
				{
					case IW_MODE_INFRA:
						mode = NETWORK_MODE_INFRA;
						break;
					case IW_MODE_ADHOC:
						mode = NETWORK_MODE_ADHOC;
						break;
					default:
						mode = NETWORK_MODE_INFRA;
						break;
				}
				nm_ap_set_mode (nm_ap, mode);
			}
			else
				nm_ap_set_mode (nm_ap, NETWORK_MODE_INFRA);

			nm_ap_set_strength (nm_ap, nm_wireless_qual_to_percent (dev, &(tmp_ap->stats.qual)));

			if (tmp_ap->b.has_freq)
				nm_ap_set_freq (nm_ap, tmp_ap->b.freq);

			g_get_current_time (&cur_time);
			nm_ap_set_last_seen (nm_ap, &cur_time);

			/* Add the AP to the device's AP list */
			if (nm_ap_list_merge_scanned_ap (nm_device_ap_list_get (dev), nm_ap))
			{
				nm_dbus_signal_wireless_network_change	(dev->app_data->dbus_connection, dev, nm_ap, FALSE);
				list_changed = TRUE;
			}
			nm_ap_unref (nm_ap);
		}
		tmp_ap = tmp_ap->next;
	}	

	/* If we detected any blank-ESSID access points (ie don't broadcast their ESSID), then try to
	 * merge in ESSIDs that we have addresses for from user preferences/NetworkManagerInfo.
	 */
	if (have_blank_essids)
		nm_ap_list_copy_essids_by_address (nm_device_ap_list_get (dev), dev->app_data->allowed_ap_list);

	/* Once we have the list, copy in any relevant information from our Allowed list. */
	nm_ap_list_copy_properties (nm_device_ap_list_get (dev), dev->app_data->allowed_ap_list);

	/* Walk the access point list and remove any access points older than 60s */
	g_get_current_time (&cur_time);
	if (nm_device_ap_list_get (dev) && (iter = nm_ap_list_iter_new (nm_device_ap_list_get (dev))))
	{
		NMAccessPoint	*outdated_ap;
		GSList		*outdated_list = NULL;
		GSList		*elem;
		char 		*essid = nm_device_get_essid (dev);

		while ((outdated_ap = nm_ap_list_iter_next (iter)))
		{
			const GTimeVal *ap_time = nm_ap_get_last_seen (outdated_ap);
			gboolean	keep_around = FALSE;

			/* We don't add an "artifical" APs to the outdated list if it is the
			 * one the card is currently associated with.
			 * Some Cisco cards don't report non-ESSID-broadcasting access points
			 * in their scans even though the card associates with that AP just fine.
			 */
			if (	    nm_ap_get_essid (outdated_ap)
				&& !strcmp (essid, nm_ap_get_essid (outdated_ap))
				&&  nm_ap_get_artificial (outdated_ap))
				keep_around = TRUE;

			/* Eh, we don't care about sub-second time resolution. */
			if ((ap_time->tv_sec + 60 < cur_time.tv_sec) && !keep_around)
				outdated_list = g_slist_append (outdated_list, outdated_ap);
		}
		nm_ap_list_iter_free (iter);

		/* Ok, now remove outdated ones.  We have to do it after the lock
		 * because nm_ap_list_remove_ap() locks the list too.
		 */
		elem = outdated_list;
		while (elem)
		{
			if ((outdated_ap = (NMAccessPoint *)(elem->data)))
			{
				nm_dbus_signal_wireless_network_change	(dev->app_data->dbus_connection, dev, outdated_ap, TRUE);
				nm_ap_list_remove_ap (nm_device_ap_list_get (dev), outdated_ap);
				list_changed = TRUE;
			}
			elem = g_slist_next (elem);
		}
		g_slist_free (outdated_list);
	}

	/* If the list changed, decrease our wireless scanning interval */
	if (list_changed)
		dev->options.wireless.scan_interval = 20;
	else
		dev->options.wireless.scan_interval = MIN (60, dev->options.wireless.scan_interval + 10);

	return FALSE;
}


/*
 * nm_device_wireless_scan
 *
 * Get a list of access points this device can see.
 *
 */
static gboolean nm_device_wireless_scan (gpointer user_data)
{
	NMDevice 				*dev = (NMDevice *)(user_data);
	int			 		 sk;
	NMWirelessScanResults	*scan_results = NULL;

	g_return_val_if_fail (dev != NULL, FALSE);
	g_return_val_if_fail (dev->app_data != NULL, FALSE);

	/* We don't really scan on test devices or devices that don't have scanning support */
	if (dev->test_device || !nm_device_supports_wireless_scan (dev))
		return FALSE;

	/* Grab the scan mutex */
	if (nm_try_acquire_mutex (dev->options.wireless.scan_mutex, __FUNCTION__))
	{
		/* Device must be up before we can scan */
		if (!nm_device_is_up (dev))
			nm_device_bring_up (dev);
		g_usleep (G_USEC_PER_SEC);

		if ((sk = iw_sockets_open ()) >= 0)
		{
			wireless_scan		*tmp_ap;
			int				 err;
			NMNetworkMode		 orig_mode = NETWORK_MODE_INFRA;
			double			 orig_freq = 0;
			int				 orig_rate = 0;

			orig_mode = nm_device_get_mode (dev);
			if (orig_mode == NETWORK_MODE_ADHOC)
			{
				orig_freq = nm_device_get_frequency (dev);
				orig_rate = nm_device_get_bitrate (dev);
			}

			/* Must be in infrastructure mode during scan, otherwise we don't get a full
			 * list of scan results.  Scanning doesn't work well in Ad-Hoc mode :( 
			 */
			nm_device_set_mode (dev, NETWORK_MODE_INFRA);
			nm_device_set_frequency (dev, 0);

			scan_results = g_malloc0 (sizeof (NMWirelessScanResults));
			err = iw_scan (sk, (char *)nm_device_get_iface (dev), WIRELESS_EXT, &(scan_results->scan_head));
			if ((err == -1) && (errno == ENODATA))
			{
				/* Card hasn't had time yet to compile full access point list.
				 * Give it some more time and scan again.  If that doesn't work
				 * give up.
				 */
				g_usleep ((G_USEC_PER_SEC * nm_device_get_association_pause_value (dev)) / 2);
				err = iw_scan (sk, (char *)nm_device_get_iface (dev), WIRELESS_EXT, &(scan_results->scan_head));
				if (err == -1)
					scan_results->scan_head.result = NULL;
			}
			else if ((err == -1) && (errno == ETIME))
				syslog (LOG_ERR, "Warning: the wireless card (%s) requires too much time for scans.  Its driver needs to be fixed.", nm_device_get_iface (dev));

			nm_device_set_mode (dev, orig_mode);
			/* Only set frequency if ad-hoc mode */
			if (orig_mode == NETWORK_MODE_ADHOC)
			{
				nm_device_set_frequency (dev, orig_freq);
				nm_device_set_bitrate (dev, orig_rate);
			}

			close (sk);
		}
		nm_unlock_mutex (dev->options.wireless.scan_mutex, __FUNCTION__);
	}

	/* We run the scan processing function from the main thread, since it must deliver
	 * messages over DBUS.  Plus, that way the main thread is the only thread that has
	 * to modify the device's access point list.
	 */
	if ((scan_results != NULL) && (scan_results->scan_head.result != NULL))
	{
		guint	 scan_process_source_id = 0;
		GSource	*scan_process_source = g_idle_source_new ();

		scan_results->dev = dev;
		g_source_set_callback (scan_process_source, nm_device_wireless_process_scan_results, scan_results, NULL);
		scan_process_source_id = g_source_attach (scan_process_source, dev->app_data->main_context);
		g_source_unref (scan_process_source);
	}

	/* Make sure we reschedule ourselves so we keep scanning */
	nm_device_wireless_schedule_scan (dev);

	return FALSE;
}


/* System config data accessors */

gboolean nm_device_config_get_use_dhcp (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, 0);

	return (dev->config_info.use_dhcp);
}

void nm_device_config_set_use_dhcp (NMDevice *dev, gboolean use_dhcp)
{
	g_return_if_fail (dev != NULL);

	dev->config_info.use_dhcp = use_dhcp;
}

guint32 nm_device_config_get_ip4_address (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, 0);

	return (dev->config_info.ip4_address);
}

void nm_device_config_set_ip4_address (NMDevice *dev, guint32 addr)
{
	g_return_if_fail (dev != NULL);

	dev->config_info.ip4_address = addr;
}

guint32 nm_device_config_get_ip4_gateway (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, 0);

	return (dev->config_info.ip4_gateway);
}

void nm_device_config_set_ip4_gateway (NMDevice *dev, guint32 gateway)
{
	g_return_if_fail (dev != NULL);

	dev->config_info.ip4_gateway = gateway;
}

guint32 nm_device_config_get_ip4_netmask (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, 0);

	return (dev->config_info.ip4_netmask);
}

void nm_device_config_set_ip4_netmask (NMDevice *dev, guint32 netmask)
{
	g_return_if_fail (dev != NULL);

	dev->config_info.ip4_netmask = netmask;
}
guint32 nm_device_config_get_ip4_broadcast (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, 0);

	return (dev->config_info.ip4_broadcast);
}

void nm_device_config_set_ip4_broadcast (NMDevice *dev, guint32 broadcast)
{
	g_return_if_fail (dev != NULL);

	dev->config_info.ip4_broadcast = broadcast;
}


/****************************************/
/* Code ripped from HAL                 */
/*   minor modifications made for       */
/* integration with NLM                 */
/****************************************/

/** Read a word from the MII transceiver management registers 
 *
 *  @param  iface               Which interface
 *  @param  location            Which register
 *  @return                     Word that is read
 */
static guint16 mdio_read (int sockfd, struct ifreq *ifr, int location, gboolean new_ioctl_nums)
{
	guint16 *data = (guint16 *) &(ifr->ifr_data);

	data[1] = location;
	if (ioctl (sockfd, new_ioctl_nums ? 0x8948 : SIOCDEVPRIVATE + 1, ifr) < 0)
	{
		syslog(LOG_ERR, "SIOCGMIIREG on %s failed: %s", ifr->ifr_name, strerror (errno));
		return -1;
	}
	return data[3];
}

static gboolean mii_get_link (NMDevice *dev)
{
	int			sockfd;
	struct ifreq	ifr;
	gboolean		new_ioctl_nums;
	guint16		status_word;
	gboolean		link_active = FALSE;

	sockfd = socket (AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0)
	{
		syslog (LOG_ERR, "cannot open socket on interface %s; errno=%d", nm_device_get_iface (dev), errno);
		return (FALSE);
	}

	snprintf (ifr.ifr_name, IFNAMSIZ, nm_device_get_iface (dev));
	if (ioctl (sockfd, 0x8947, &ifr) >= 0)
		new_ioctl_nums = TRUE;
	else if (ioctl (sockfd, SIOCDEVPRIVATE, &ifr) >= 0)
		new_ioctl_nums = FALSE;
	else
	{
		syslog (LOG_ERR, "SIOCGMIIPHY on %s failed: %s", ifr.ifr_name, strerror (errno));
		close (sockfd);
		return (FALSE);
	}

	/* Refer to http://www.scyld.com/diag/mii-status.html for
	 * the full explanation of the numbers
	 *
	 * 0x8000  Capable of 100baseT4.
	 * 0x7800  Capable of 10/100 HD/FD (most common).
	 * 0x0040  Preamble suppression permitted.
	 * 0x0020  Autonegotiation complete.
	 * 0x0010  Remote fault.
	 * 0x0008  Capable of Autonegotiation.
	 * 0x0004  Link established ("sticky"* on link failure)
	 * 0x0002  Jabber detected ("sticky"* on transmit jabber)
	 * 0x0001  Extended MII register exist.
	 *
	 */

	/* We have to read it twice to clear any "sticky" bits */
	status_word = mdio_read (sockfd, &ifr, 1, new_ioctl_nums);
	status_word = mdio_read (sockfd, &ifr, 1, new_ioctl_nums);

	if ((status_word & 0x0016) == 0x0004)
		link_active = TRUE;
	else
		link_active = FALSE;

	close (sockfd);

	return (link_active);
}

/****************************************/
/* End Code ripped from HAL             */
/****************************************/


/****************************************/
/* Test device routes                   */
/****************************************/

/*
 * nm_device_is_test_device
 *
 */
gboolean nm_device_is_test_device (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, FALSE);

	return (dev->test_device);
}
