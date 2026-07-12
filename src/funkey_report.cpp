#include "funkey_report.hpp"

#include <string.h>

#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_report_mux = portMUX_INITIALIZER_UNLOCKED;
static const uint8_t s_removed_report[FUNKEY_REPORT_LEN] = {
    0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x00, 0x00,
};
static uint8_t s_current_report[FUNKEY_REPORT_LEN] = {
    0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x00, 0x00,
};
#if FUNKEY_PHYSICAL_ENABLED
static const uint8_t s_physical_contact_issue_report[FUNKEY_REPORT_LEN] = {
    0xFF, 0xFF, 0xFF, 0xF1, 0x00, 0x00, 0x00, 0x01,
};
static uint8_t s_physical_report[FUNKEY_REPORT_LEN] = {
    0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x00, 0x00,
};
#endif

static const uint8_t s_raw_no_figure[FUNKEY_PORTAL_PACKET_LEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
};
static const uint16_t s_portal_baseline_adc = 177;
static const uint16_t s_portal_digit_adc[10] = {
    278, 375, 465, 554, 643, 713, 783, 844, 899, 942,
};
static uint8_t s_portal_stable_packet[FUNKEY_PORTAL_PACKET_LEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
};

static funkey::report::NotifyFn s_report_notify;
static funkey::report::NotifyFn s_physical_report_notify;

void funkey::report::setNotifyCallbacks(NotifyFn reportNotify, NotifyFn physicalReportNotify)
{
    s_report_notify = reportNotify;
    s_physical_report_notify = physicalReportNotify;
}

void funkey::report::copy(uint8_t out[FUNKEY_REPORT_LEN])
{
    portENTER_CRITICAL(&s_report_mux);
    memcpy(out, s_current_report, FUNKEY_REPORT_LEN);
    portEXIT_CRITICAL(&s_report_mux);
}

#if FUNKEY_PHYSICAL_ENABLED
void funkey::physical_report::copy(uint8_t out[FUNKEY_REPORT_LEN])
{
    portENTER_CRITICAL(&s_report_mux);
    memcpy(out, s_physical_report, FUNKEY_REPORT_LEN);
    portEXIT_CRITICAL(&s_report_mux);
}
#endif

bool funkey::report::isRemoved(const uint8_t report[FUNKEY_REPORT_LEN])
{
    return memcmp(report, s_removed_report, FUNKEY_REPORT_LEN) == 0;
}

#if FUNKEY_PHYSICAL_ENABLED
bool funkey::physical_report::isContactIssue(const uint8_t report[FUNKEY_REPORT_LEN])
{
    return memcmp(report, s_physical_contact_issue_report, FUNKEY_REPORT_LEN) == 0;
}
#endif

uint32_t funkey::report::id(const uint8_t report[FUNKEY_REPORT_LEN])
{
    return ((uint32_t)report[4] << 24) | ((uint32_t)report[5] << 16) |
           ((uint32_t)report[6] << 8) | (uint32_t)report[7];
}

void funkey::report::fromId(uint32_t id, uint8_t out[FUNKEY_REPORT_LEN])
{
    out[0] = 0xFF;
    out[1] = 0xFF;
    out[2] = 0xFF;
    out[3] = 0xF0;
    out[4] = (uint8_t)(id >> 24);
    out[5] = (uint8_t)(id >> 16);
    out[6] = (uint8_t)(id >> 8);
    out[7] = (uint8_t)id;
}

static void portal_set_stable_packet_locked(const uint8_t packet[FUNKEY_PORTAL_PACKET_LEN])
{
    memcpy(s_portal_stable_packet, packet, FUNKEY_PORTAL_PACKET_LEN);
}

bool funkey::portal::packetFromId(uint32_t id, uint8_t out[FUNKEY_PORTAL_PACKET_LEN])
{
    if (id > 999) {
        return false;
    }

    uint8_t digits[4] = {
        static_cast<uint8_t>(id % 10),
        static_cast<uint8_t>((id / 10) % 10),
        static_cast<uint8_t>((id / 100) % 10),
        0,
    };
    digits[3] = static_cast<uint8_t>((digits[0] + digits[1] + digits[2]) % 10);

    memset(out, 0, FUNKEY_PORTAL_PACKET_LEN);
    for (size_t i = 0; i < 4; ++i) {
        uint16_t adc = s_portal_digit_adc[digits[i]];
        out[i] = adc & 0xFF;
        out[4] |= ((adc >> 8) & 0x03) << (i * 2);
    }

    out[5] = s_portal_baseline_adc & 0xFF;
    out[6] = (s_portal_baseline_adc >> 8) & 0xFF;
    return true;
}

static void portal_packet_for_report_locked(const uint8_t report[FUNKEY_REPORT_LEN])
{
    if (funkey::report::isRemoved(report)) {
        portal_set_stable_packet_locked(s_raw_no_figure);
        return;
    }

    uint8_t packet[FUNKEY_PORTAL_PACKET_LEN];
    if (funkey::portal::packetFromId(funkey::report::id(report), packet)) {
        portal_set_stable_packet_locked(packet);
    } else {
        portal_set_stable_packet_locked(s_raw_no_figure);
    }
}

void funkey::report::set(const uint8_t in[FUNKEY_REPORT_LEN])
{
    portENTER_CRITICAL(&s_report_mux);
    memcpy(s_current_report, in, FUNKEY_REPORT_LEN);
    portal_packet_for_report_locked(in);
    portEXIT_CRITICAL(&s_report_mux);

    if (s_report_notify != nullptr) {
        s_report_notify();
    }
}

void funkey::report::setId(uint32_t id)
{
    uint8_t report[FUNKEY_REPORT_LEN];
    funkey::report::fromId(id, report);
    funkey::report::set(report);
}

#if FUNKEY_PHYSICAL_ENABLED
void funkey::physical_report::set(const uint8_t in[FUNKEY_REPORT_LEN])
{
    portENTER_CRITICAL(&s_report_mux);
    memcpy(s_physical_report, in, FUNKEY_REPORT_LEN);
    portEXIT_CRITICAL(&s_report_mux);

    if (s_physical_report_notify != nullptr) {
        s_physical_report_notify();
    }
}

void funkey::physical_report::setId(uint32_t id)
{
    uint8_t report[FUNKEY_REPORT_LEN];
    funkey::report::fromId(id, report);
    funkey::physical_report::set(report);
}

void funkey::physical_report::remove()
{
    funkey::physical_report::set(s_removed_report);
}

void funkey::physical_report::contactIssue()
{
    funkey::physical_report::set(s_physical_contact_issue_report);
}
#endif

void funkey::report::remove()
{
    funkey::report::set(s_removed_report);
}

void funkey::portal::setRawPacket(const uint8_t packet[FUNKEY_PORTAL_PACKET_LEN])
{
    portENTER_CRITICAL(&s_report_mux);
    portal_set_stable_packet_locked(packet);
    portEXIT_CRITICAL(&s_report_mux);
}

void funkey::portal::copyStablePacket(uint8_t out[FUNKEY_PORTAL_PACKET_LEN])
{
    portENTER_CRITICAL(&s_report_mux);
    memcpy(out, s_portal_stable_packet, FUNKEY_PORTAL_PACKET_LEN);
    portEXIT_CRITICAL(&s_report_mux);
}
