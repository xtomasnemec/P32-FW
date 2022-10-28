/**
 * @file selftest_result_heaters.cpp
 * @author Radek Vana
 * @date 2022-01-23
 */

#include "selftest_result_heaters.hpp"
#include "i18n.h"
#include "resource.h"

ResultHeaters::ResultHeaters(TestResult_t res_noz, TestResult_t res_bed)
    : SelfTestGroup(_("Heaters check"))
    , noz(_("Nozzle"), png::Get<png::Id::nozzle_16x16>(), res_noz)
    , bed(_("Heatbed"), png::Get<png::Id::heatbed_16x16>(), res_bed) {
    Add(noz);
    Add(bed);
    if (res_noz == TestResult_t::Failed || res_bed == TestResult_t::Failed) {
        failed = true;
    }
}
