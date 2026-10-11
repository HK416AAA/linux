/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CHARLOTTE_WIFI_CAPS_H
#define CHARLOTTE_WIFI_CAPS_H

#include <linux/slab.h>
#include <linux/unaligned.h>
#include "mac_mib.h"
#include "mac_frame.h"
#include "mac_function.h"
#include "hmac_11ax.h"

/* Use the same board/device MIB and IE builders as association/beacon frames.
 * The vendor's empty wiphy templates otherwise advertise no HT MCS, eight VHT
 * streams, and no HE on the two bands this board actually supports. These
 * temporary objects have no firmware VAP, resource-pool entry or timers.
 */
static struct ieee80211_sband_iftype_data charlotte_he_caps[2][2];

static int charlotte_copy_he_cap(struct ieee80211_sta_he_cap *cap,
                                const u8 *ie, unsigned int len)
{
    const struct ieee80211_he_cap_elem *elem;
    unsigned int mcs_len, ppe_len = 0, fixed = sizeof(*elem);
    const u8 *data = ie + 3;

    memset(cap, 0, sizeof(*cap));
    if (!len)
        return 0;
    if (len < 3 + fixed || ie[0] != WLAN_EID_EXTENSION ||
        ie[2] != WLAN_EID_EXT_HE_CAPABILITY)
        return -EINVAL;
    elem = (const void *)data;
    mcs_len = ieee80211_he_mcs_nss_size(elem);
    if (3 + fixed + mcs_len > len)
        return -EINVAL;
    if (elem->phy_cap_info[6] & IEEE80211_HE_PHY_CAP6_PPE_THRESHOLD_PRESENT) {
        if (3 + fixed + mcs_len >= len)
            return -EINVAL;
        ppe_len = ieee80211_he_ppe_size(data[fixed + mcs_len], elem->phy_cap_info);
    }
    if (3 + fixed + mcs_len + ppe_len > len ||
        ppe_len > sizeof(cap->ppe_thres))
        return -EINVAL;
    memcpy(&cap->he_cap_elem, elem, fixed);
    /* Unsupported bandwidth maps must be 0xffff, not eight MCS0-7 streams. */
    memset(&cap->he_mcs_nss_supp, 0xff, sizeof(cap->he_mcs_nss_supp));
    memcpy(&cap->he_mcs_nss_supp, data + fixed, mcs_len);
    if (ppe_len)
        memcpy(cap->ppe_thres, data + fixed + mcs_len, ppe_len);
    cap->has_he = true;
    return 0;
}

static int charlotte_init_band_caps(struct wiphy *wiphy, u8 dev_id)
{
    mac_device_stru *device = mac_res_get_dev(dev_id);
    mac_vap_stru *vap;
    wlan_mib_ieee802dot11_stru *mib;
    struct ieee80211_supported_band *band;
    u8 ie[128], len;
    int b, mode, ret = 0;

    if (!device)
        return -ENODEV;
    vap = kzalloc(sizeof(*vap), GFP_KERNEL);
    mib = kzalloc(sizeof(*mib), GFP_KERNEL);
    if (!vap || !mib) {
        ret = -ENOMEM;
        goto out;
    }
    for (b = NL80211_BAND_2GHZ; b <= NL80211_BAND_5GHZ; b++) {
        band = wiphy->bands[b];
        if (!band)
            continue;
        for (mode = 0; mode < 2; mode++) {
            memset(vap, 0, sizeof(*vap));
            memset(mib, 0, sizeof(*mib));
            vap->pst_mib_info = mib;
            vap->uc_device_id = dev_id;
            vap->uc_chip_id = device->uc_chip_id;
            vap->en_vap_mode = mode ? WLAN_VAP_MODE_BSS_AP : WLAN_VAP_MODE_BSS_STA;
            vap->en_p2p_mode = WLAN_LEGACY_VAP_MODE;
            vap->en_vap_wmm = OAL_TRUE;
            vap->st_channel.en_band = b == NL80211_BAND_2GHZ ? WLAN_BAND_2G : WLAN_BAND_5G;
            vap->st_channel.en_bandwidth = b == NL80211_BAND_2GHZ ?
                WLAN_BAND_WIDTH_40PLUS : WLAN_BAND_WIDTH_80PLUSPLUS;
            if (b == NL80211_BAND_5GHZ && mac_device_get_cap_bw(device) == WLAN_BW_CAP_160M)
                vap->st_channel.en_bandwidth = WLAN_BAND_WIDTH_160PLUSPLUSPLUS;
            mac_vap_11ax_cap_init(vap);
            mac_init_mib(vap);
            mac_mib_set_vht_max_rx_ampdu_factor(vap, MAC_VHT_AMPDU_MAX_LEN_EXP);
            ret = mac_vap_init_by_protocol(vap, g_wlan_spec_cfg->feature_11ax_is_open &&
                vap->en_11ax_custom_switch ? WLAN_HE_MODE : WLAN_VHT_MODE);
            if (ret != OAL_SUCC) {
                ret = -EINVAL;
                goto out;
            }
            mac_vap_change_mib_by_bandwidth(vap, vap->st_channel.en_band,
                                          vap->st_channel.en_bandwidth);
            mac_vap_init_rates(vap);
            if (!mode) {
                len = mac_set_ht_capabilities_ie(vap, ie);
                if (len != 2 + sizeof(struct ieee80211_ht_cap)) {
                    pr_err("charlotte-wifi: HT IE band=%d mode=%d len=%u expected=%zu\n",
                           b, mode, len, 2 + sizeof(struct ieee80211_ht_cap));
                    ret = -EINVAL;
                    goto out;
                }
                band->ht_cap.ht_supported = true;
                band->ht_cap.cap = get_unaligned_le16(ie + 2);
                band->ht_cap.ampdu_factor = ie[4] & IEEE80211_HT_AMPDU_PARM_FACTOR;
                band->ht_cap.ampdu_density = (ie[4] & IEEE80211_HT_AMPDU_PARM_DENSITY) >>
                                            IEEE80211_HT_AMPDU_PARM_DENSITY_SHIFT;
                memcpy(&band->ht_cap.mcs, ie + 5, sizeof(band->ht_cap.mcs));
                if (b == NL80211_BAND_5GHZ) {
                    mac_set_vht_capabilities_ie(vap, ie, &len);
                    if (len != 2 + sizeof(struct ieee80211_vht_cap)) {
                        pr_err("charlotte-wifi: VHT IE band=%d mode=%d len=%u expected=%zu\n",
                               b, mode, len, 2 + sizeof(struct ieee80211_vht_cap));
                        ret = -EINVAL;
                        goto out;
                    }
                    band->vht_cap.vht_supported = true;
                    band->vht_cap.cap = get_unaligned_le32(ie + 2);
                    memcpy(&band->vht_cap.vht_mcs, ie + 6, sizeof(band->vht_cap.vht_mcs));
                }
            }
            charlotte_he_caps[b][mode].types_mask = mode ?
                BIT(NL80211_IFTYPE_AP) | BIT(NL80211_IFTYPE_P2P_GO) :
                BIT(NL80211_IFTYPE_STATION) | BIT(NL80211_IFTYPE_P2P_CLIENT);
            mac_set_he_capabilities_ie(vap, ie, &len);
            ret = charlotte_copy_he_cap(&charlotte_he_caps[b][mode].he_cap, ie, len);
            if (ret) {
                pr_err("charlotte-wifi: HE IE band=%d mode=%d len=%u: %*ph\n",
                       b, mode, len, len, ie);
                goto out;
            }
        }
        band->iftype_data = charlotte_he_caps[b];
        band->n_iftype_data = ARRAY_SIZE(charlotte_he_caps[b]);
    }
out:
    kfree(mib);
    kfree(vap);
    return ret;
}
#endif
