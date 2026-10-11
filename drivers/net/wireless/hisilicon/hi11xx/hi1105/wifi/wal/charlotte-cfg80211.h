/* SPDX-License-Identifier: GPL-2.0-only */
/* Linux 7.3 callbacks around the vendor's single-link WLAN implementation. */
#ifndef CHARLOTTE_CFG80211_H
#define CHARLOTTE_CFG80211_H

static int charlotte_add_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			     int link_id, u8 idx, bool pairwise, const u8 *mac,
			     struct key_params *params)
{
	if (!wdev->netdev || link_id > 0)
		return -EOPNOTSUPP;
	return wal_cfg80211_add_key(wiphy, wdev->netdev, idx, pairwise, mac, params);
}

static int charlotte_get_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			     int link_id, u8 idx, bool pairwise, const u8 *mac,
			     void *cookie, void (*callback)(void *, struct key_params *))
{
	if (!wdev->netdev || link_id > 0)
		return -EOPNOTSUPP;
	return wal_cfg80211_get_key(wiphy, wdev->netdev, idx, pairwise, mac, cookie, callback);
}

static int charlotte_del_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			     int link_id, u8 idx, bool pairwise, const u8 *mac)
{
	if (!wdev->netdev || link_id > 0)
		return -EOPNOTSUPP;
	return wal_cfg80211_remove_key(wiphy, wdev->netdev, idx, pairwise, mac);
}

static int charlotte_default_key(struct wiphy *wiphy, struct net_device *dev,
				 int link_id, u8 idx, bool unicast, bool multicast)
{
	if (link_id > 0)
		return -EOPNOTSUPP;
	return wal_cfg80211_set_default_key(wiphy, dev, idx, unicast, multicast);
}

static int charlotte_default_mgmt_key(struct wiphy *wiphy, struct wireless_dev *wdev,
				      int link_id, u8 idx)
{
	if (!wdev->netdev || link_id > 0)
		return -EOPNOTSUPP;
	return wal_cfg80211_set_default_mgmt_key(wiphy, wdev->netdev, idx);
}

static int charlotte_wiphy_params(struct wiphy *wiphy, int radio_idx, u32 changed)
{
	if (radio_idx > 0)
		return -EOPNOTSUPP;
	return wal_cfg80211_set_wiphy_params(wiphy, changed);
}

static int charlotte_change_beacon(struct wiphy *wiphy, struct net_device *dev,
				   struct cfg80211_ap_update *info)
{
	if (info->beacon.link_id > 0)
		return -EOPNOTSUPP;
	return wal_cfg80211_change_beacon(wiphy, dev, &info->beacon);
}

static int charlotte_stop_ap(struct wiphy *wiphy, struct net_device *dev,
			     unsigned int link_id)
{
	if (link_id)
		return -EOPNOTSUPP;
	return wal_cfg80211_stop_ap(wiphy, dev);
}

static int charlotte_add_station(struct wiphy *wiphy, struct wireless_dev *wdev,
				 const u8 *mac, struct station_parameters *params)
{
	if (!wdev->netdev)
		return -EOPNOTSUPP;
	return wal_cfg80211_add_station(wiphy, wdev->netdev, mac, params);
}

static int charlotte_del_station(struct wiphy *wiphy, struct wireless_dev *wdev,
				 struct station_del_parameters *params)
{
	if (!wdev->netdev)
		return -EOPNOTSUPP;
	return wal_cfg80211_del_station(wiphy, wdev->netdev, params);
}

static int charlotte_change_station(struct wiphy *wiphy, struct wireless_dev *wdev,
				    const u8 *mac, struct station_parameters *params)
{
	if (!wdev->netdev)
		return -EOPNOTSUPP;
	return wal_cfg80211_change_station(wiphy, wdev->netdev, mac, params);
}

static int charlotte_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,
				 const u8 *mac, struct station_info *info)
{
	if (!wdev->netdev)
		return -EOPNOTSUPP;
	return wal_cfg80211_get_station(wiphy, wdev->netdev, mac, info);
}

static int charlotte_dump_station(struct wiphy *wiphy, struct wireless_dev *wdev,
				  int idx, u8 *mac, struct station_info *info)
{
	if (!wdev->netdev)
		return -EOPNOTSUPP;
	return wal_cfg80211_dump_station(wiphy, wdev->netdev, idx, mac, info);
}

static int charlotte_bitrate_mask(struct wiphy *wiphy, struct net_device *dev,
				  unsigned int link_id, const u8 *peer,
				  const struct cfg80211_bitrate_mask *mask)
{
	if (link_id)
		return -EOPNOTSUPP;
	return wal_cfg80211_set_bitrate_mask(wiphy, dev, peer, mask);
}

static int charlotte_mgmt_tx(struct wiphy *wiphy, struct wireless_dev *wdev,
			     struct cfg80211_mgmt_tx_params *params, u64 cookie)
{
	return wal_cfg80211_mgmt_tx(wiphy, wdev, params, &cookie);
}

static int charlotte_remain_on_channel(struct wiphy *wiphy,
				       struct wireless_dev *wdev,
				       struct ieee80211_channel *chan,
				       unsigned int duration, u64 cookie,
				       const u8 *peer)
{
	if (peer)
		return -EOPNOTSUPP;
	return wal_cfg80211_remain_on_channel(wiphy, wdev, chan, duration, &cookie);
}

#endif
