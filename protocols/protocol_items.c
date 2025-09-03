#include "protocol_items.h"

const SubGhzProtocol* tpms_protocol_registry_items[] = {
    &tpms_protocol_schrader_gg4,
    &tpms_protocol_toyota,
    &tpms_protocol_ford,
    &tpms_protocol_gm,
    &tpms_protocol_nissan,
    &tpms_protocol_hyundai,
};

const SubGhzProtocolRegistry tpms_protocol_registry = {
    .items = tpms_protocol_registry_items,
    .size = COUNT_OF(tpms_protocol_registry_items)};
