#include <attributes_info.H>

#include <guard_with_eid_records.hpp>
#include <libguard/guard_interface.hpp>
#include <phosphor-logging/lg2.hpp>
#include <util.hpp>
extern "C"
{
#include <libpdbg.h>
}

namespace openpower::faultlog
{
using ::nlohmann::json;

constexpr auto stateConfigured = "CONFIGURED";
constexpr auto stateDeconfigured = "DECONFIGURED";

/** Data structure to pass to pdbg_target_traverse callback method*/
struct GuardedTarget
{
    pdbg_target* target = nullptr;
    std::string phyDevPath = {};
    GuardedTarget(const std::string& path) : phyDevPath(path)
    {}
};

using PropertyValue =
    std::variant<std::string, bool, uint8_t, int16_t, uint16_t, int32_t,
                 uint32_t, int64_t, uint64_t, double>;

using Properties = std::map<std::string, PropertyValue>;
/**
 * @brief Get PDBG target matching the guarded target physicalpath
 *
 * This callback function is called as part of the recursive method
 * pdbg_target_traverse, recursion will exit when the method return 1
 * else continues till the target is found
 *
 * @param[in] target - pdbg target to compare
 * @param[inout] priv - data structure passed to the callback method
 *
 * @return 1 when target is found else 0
 */
static int getGuardedTarget(struct pdbg_target* target, void* priv)
{
    GuardedTarget* guardTarget = reinterpret_cast<GuardedTarget*>(priv);
    ATTR_PHYS_DEV_PATH_Type phyPath;
    if (!DT_GET_PROP(ATTR_PHYS_DEV_PATH, target, phyPath))
    {
        if (strcmp(phyPath, guardTarget->phyDevPath.c_str()) == 0)
        {
            guardTarget->target = target;
            return 1;
        }
    }
    return 0;
}

int GuardWithEidRecords::getCount(const GuardRecords& guardRecords)
{
    int count = 0;
    for (const auto& elem : guardRecords)
    {
        // ignore manual guard records
        if (elem.elogId == 0)
        {
            continue;
        }
        count += 1;
    }
    return count;
}

void GuardWithEidRecords::populate(sdbusplus::bus::bus& bus,
                                   const GuardRecords& guardRecords,
                                   json& jsonNag)
{

    for (const auto& elem : guardRecords)
    {
        try
        {
            // ignore manual guard records
            if (elem.elogId == 0)
            {
                continue;
            }
            uint32_t bmcLogId = 0;
            bool dbusErrorObjFound = true;
            try
            {
                auto method = bus.new_method_call(
                    "xyz.openbmc_project.Logging",
                    "/xyz/openbmc_project/logging",
                    "org.open_power.Logging.PEL", "GetBMCLogIdFromPELId");

                method.append(static_cast<uint32_t>(elem.elogId));
                auto resp = bus.call(method);
                resp.read(bmcLogId);
            }
            catch (const sdbusplus::exception::SdBusError& ex)
            {
                dbusErrorObjFound = false;
                lg2::info(
                    "PEL might be deleted but guard entry is around {ELOG_ID)",
                    "ELOG_ID", elem.elogId);
            }

            json jsonErrorLog = json::object();
            json jsonErrorLogSection = json::array();
            if (dbusErrorObjFound)
            {
                std::string callouts;
                std::string refCode;
                std::string objPath = "/xyz/openbmc_project/logging/entry/" +
                                      std::to_string(bmcLogId);
                Properties loggingEntryProp;
                auto loggingEntryMethod = bus.new_method_call(
                    "xyz.openbmc_project.Logging", objPath.c_str(),
                    "org.freedesktop.DBus.Properties", "GetAll");
                loggingEntryMethod.append("xyz.openbmc_project.Logging.Entry");
                auto loggingEntryReply = bus.call(loggingEntryMethod);
                loggingEntryReply.read(loggingEntryProp);
                for (const auto& [prop, propValue] : loggingEntryProp)
                {
                    if (prop == "Resolution")
                    {
                        auto calloutsPtr = std::get_if<std::string>(&propValue);
                        if (calloutsPtr != nullptr)
                        {
                            callouts = *calloutsPtr;
                        }
                    }
                    else if (prop == "EventId")
                    {
                        auto eventIdPtr = std::get_if<std::string>(&propValue);
                        if (eventIdPtr != nullptr)
                        {
                            std::istringstream iss(*eventIdPtr);
                            iss >> refCode;
                        }
                    }
                } // endfor
                uint32_t plid = 0;
                uint64_t timestamp = 0;
                Properties pelEntryProp;
                auto pelEntryMethod = bus.new_method_call(
                    "xyz.openbmc_project.Logging", objPath.c_str(),
                    "org.freedesktop.DBus.Properties", "GetAll");
                pelEntryMethod.append("org.open_power.Logging.PEL.Entry");
                auto pelEntryReply = bus.call(pelEntryMethod);
                pelEntryReply.read(pelEntryProp);
                for (const auto& [prop, propValue] : pelEntryProp)
                {
                    if (prop == "PlatformLogID")
                    {
                        auto plidPtr = std::get_if<uint32_t>(&propValue);
                        if (plidPtr != nullptr)
                        {
                            plid = *plidPtr;
                        }
                    }
                    else if (prop == "Timestamp")
                    {
                        auto timestampPtr = std::get_if<uint64_t>(&propValue);
                        if (timestampPtr != nullptr)
                        {
                            timestamp = *timestampPtr;
                        }
                    }
                }
                std::stringstream ss;
                ss << std::hex << "0x" << plid;
                jsonErrorLog["PLID"] = ss.str();
                jsonErrorLog["Callout Section"] = parseCallout(callouts);
                jsonErrorLog["SRC"] = refCode;
                jsonErrorLog["DATE_TIME"] = epochTimeToBCD(timestamp);
            }

            // add resource actions section
            auto physicalPath =
                openpower::guard::getPhysicalPath(elem.targetId);
            if (!physicalPath.has_value())
            {
                lg2::error("Failed to get physical path for record {RECORD_ID}",
                           "RECORD_ID", elem.recordId);
                continue;
            }

            GuardedTarget guardedTarget(*physicalPath);
            pdbg_target_traverse(nullptr, getGuardedTarget, &guardedTarget);
            if (guardedTarget.target == nullptr)
            {
                lg2::error("Failed to find the pdbg target for the guarded "
                           "target {RECORD_ID}",
                           "RECORD_ID", elem.recordId);
                continue;
            }
            json jsonResource = json::object();
            jsonResource["TYPE"] = pdbgTargetName(guardedTarget.target);
            std::string state = stateDeconfigured;
            ATTR_HWAS_STATE_Type hwasState;
            if (!DT_GET_PROP(ATTR_HWAS_STATE, guardedTarget.target, hwasState))
            {
                if (hwasState.functional)
                {
                    state = stateConfigured;
                }
            }
            jsonResource["CURRENT_STATE"] = std::move(state);

            jsonResource["REASON_DESCRIPTION"] =
                getGuardReason(guardRecords, *physicalPath);

            jsonResource["GARD_RECORD"] = true;

            // error object is deleted add what ever data is found
            if (!dbusErrorObjFound)
            {
                json jsonCallout = json::object();
                json sectionJson = json::object();
                ATTR_LOCATION_CODE_Type attrLocCode;
                if (!DT_GET_PROP(ATTR_LOCATION_CODE, guardedTarget.target,
                                 attrLocCode))
                {
                    jsonCallout["Location Code"] = attrLocCode;
                }
                sectionJson["Callout Count"] = 1;
                sectionJson["Callouts"] = jsonCallout;
                jsonErrorLog["PLID"] =
                    std::to_string(hwasState.deconfiguredByEid);
                jsonErrorLog["Callout Section"] = sectionJson;
                jsonErrorLog["SRC"] = 0;
                jsonErrorLog["DATE_TIME"] = "00/00/0000 00:00:00";
            }

            jsonErrorLogSection.push_back(std::move(jsonErrorLog));
            json jsonEventData = json::object();
            jsonEventData["RESOURCE_ACTIONS"] = std::move(jsonResource);
            jsonErrorLogSection.push_back(jsonEventData);

            json jsonErrlogObj = json::object();
            jsonErrlogObj["CEC_ERROR_LOG"] = std::move(jsonErrorLogSection);

            json jsonServiceEvent = json::object();
            jsonServiceEvent["SERVICABLE_EVENT"] = std::move(jsonErrlogObj);
            jsonNag.push_back(std::move(jsonServiceEvent));
        }
        catch (const std::exception& ex)
        {
            lg2::info("Failed to add guard record {ELOG_ID}, {ERROR}",
                      "ELOG_ID", elem.elogId, "ERROR", ex);
        }
    }
}
} // namespace openpower::faultlog
