#include <vector>
#include <stdlib.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <host-ipmid/ipmid-api.h>
#include <iostream>
#include <memory>
#include <algorithm>
#include <fstream>
#include <sstream>
#include "frup.h"
#include "fru-area.H"

// OpenBMC System Manager dbus framework
const char  *sys_bus_name      =  "org.openbmc.managers.System";
const char  *sys_object_name   =  "/org/openbmc/managers/System";
const char  *sys_intf_name     =  "org.openbmc.managers.System";

//----------------------------------------------------------------
// Constructor
//----------------------------------------------------------------
ipmi_fru::ipmi_fru(const uint8_t fruid, const ipmi_fru_area_type type,
                   sd_bus *bus_type, bool bmc_fru)
{
    iv_fruid = fruid;
    iv_type = type;
    iv_bmc_fru = bmc_fru;
    iv_bus_type = bus_type;
    iv_valid = false;
    iv_data = NULL;
    iv_present = false;

    if(iv_type == IPMI_FRU_AREA_INTERNAL_USE)
    {
        iv_name = "INTERNAL_";
    }
    else if(iv_type == IPMI_FRU_AREA_CHASSIS_INFO)
    {
        iv_name = "CHASSIS_";
    }
    else if(iv_type == IPMI_FRU_AREA_BOARD_INFO)
    {
        iv_name = "BOARD_";
    }
    else if(iv_type == IPMI_FRU_AREA_PRODUCT_INFO)
    {
        iv_name = "PRODUCT_";
    }
    else if(iv_type == IPMI_FRU_AREA_MULTI_RECORD)
    {
        iv_name = "MULTI_";
    }
    else
    {
        iv_name = IPMI_FRU_AREA_TYPE_MAX;
        fprintf(stderr, "ERROR: Invalid Area type :[%d]\n",iv_type);
    }
}

//-----------------------------------------------------
// For a FRU area type, accepts the data and updates
// area specific data.
//-----------------------------------------------------
void ipmi_fru::set_data(const uint8_t *data, const size_t len)
{
    iv_len = len;
    iv_data = new uint8_t[len];
    memcpy(iv_data, data, len);
}

//-----------------------------------------------------
// Sets the dbus parameters
//-----------------------------------------------------
void ipmi_fru::update_dbus_paths(const char *bus_name,
                      const char *obj_path, const char *intf_name)
{
    iv_bus_name = bus_name;
    iv_obj_path = obj_path;
    iv_intf_name = intf_name;
}

//-------------------
// Destructor
//-------------------
ipmi_fru::~ipmi_fru()
{
    sd_bus_error bus_error = SD_BUS_ERROR_NULL;
    sd_bus_message *response = NULL;
    int rc = 0;

    if(iv_data != NULL)
    {
        delete [] iv_data;
        iv_data = NULL;
    }

    // If we have not been successful in doing some updates and we are a BMC
    // fru, then need to set the fault bits.
    bool valid_dbus = !(iv_bus_name.empty()) &&
                      !(iv_obj_path.empty()) &&
                      !(iv_intf_name.empty());

    // Based on bmc_fru, success in updating the FRU inventory we need to set
    // some special bits.
    if(iv_bmc_fru && valid_dbus)
    {
        // Set the Fault bit if we did not successfully process the fru
        const char *fault_bit = iv_valid ? "False" : "True";

        rc = sd_bus_call_method(iv_bus_type,                // On the System Bus
                                iv_bus_name.c_str(),        // Service to contact
                                iv_obj_path.c_str(),        // Object path
                                iv_intf_name.c_str(),       // Interface name
                                "setFault",                 // Method to be called
                                &bus_error,                 // object to return error
                                &response,                  // Response message on success
                                "s",                        // input message (string)
                                fault_bit);                 // First argument to setFault

        if(rc <0)
        {
            fprintf(stderr,"Failed to set Fault bit, value:[%s] for fruid:[%d], path:[%s]\n",
                    fault_bit, iv_fruid, iv_obj_path.c_str());
        }
        else
        {
            printf("Fault bit set to :[%s] for fruid:[%d], Path:[%s]\n",
                    fault_bit, iv_fruid,iv_obj_path.c_str());
        }

        sd_bus_error_free(&bus_error);
        sd_bus_message_unref(response);

        // Set the Present bits
        const char *present_bit = iv_present ? "True" : "False";

        rc = sd_bus_call_method(iv_bus_type,                // On the System Bus
                                iv_bus_name.c_str(),        // Service to contact
                                iv_obj_path.c_str(),        // Object path
                                iv_intf_name.c_str(),       // Interface name
                                "setPresent",               // Method to be called
                                &bus_error,                 // object to return error
                                &response,                  // Response message on success
                                "s",                        // input message (string)
                                present_bit);               // First argument to setPresent
        if(rc < 0)
        {
            fprintf(stderr,"Failed to set Present bit for fruid:[%d], path:[%s]\n",
                    iv_fruid, iv_obj_path.c_str());
        }
        else
        {
            printf("Present bit set to :[%s] for fruid:[%d], Path[%s]:\n",
                    present_bit, iv_fruid, iv_obj_path.c_str());
        }

        sd_bus_error_free(&bus_error);
        sd_bus_message_unref(response);
    }
}

// Sets up the sd_bus structures for the given fru type
int ipmi_fru::setup_sd_bus_paths(void)
{
    // Need this to get respective DBUS objects
    sd_bus_error bus_error = SD_BUS_ERROR_NULL;
    sd_bus_message *response = NULL;
    int rc = 0;

    // What we need is BOARD_1, PRODUCT_1, CHASSIS_1 etc..
    char *inv_bus_name, *inv_obj_path, *inv_intf_name;
    char fru_area_name[16] = {0};
    sprintf(fru_area_name,"%s%d",iv_name.c_str(), iv_fruid);

#ifdef __IPMI_DEBUG__
    printf("Getting sd_bus for :[%s]\n",fru_area_name);
#endif

    // We want to call a method "getObjectFromId" on System Bus that is
    // made available over  OpenBmc system services.

    rc = sd_bus_call_method(iv_bus_type,                // On the System Bus
                            sys_bus_name,               // Service to contact
                            sys_object_name,            // Object path
                            sys_intf_name,              // Interface name
                            "getObjectFromId",          // Method to be called
                            &bus_error,                 // object to return error
                            &response,                  // Response message on success
                            "ss",                       // input message (string,string)
                            "FRU_STR",                  // First argument to getObjectFromId
                            fru_area_name);             // Second Argument
    if(rc < 0)
    {
        fprintf(stderr, "Failed to resolve fruid:[%d] to dbus: [%s]\n", iv_fruid, bus_error.message);
    }
    else
    {
        // Method getObjectFromId returns 3 parameters and all are strings, namely
        // bus_name , object_path and interface name for accessing that particular
        // FRU over Inventory SDBUS manager. 'sss' here mentions that format.
        rc = sd_bus_message_read(response, "(sss)", &inv_bus_name, &inv_obj_path, &inv_intf_name);
        if(rc < 0)
        {
            fprintf(stderr, "Failed to parse response message:[%s]\n", strerror(-rc));
        }
        else
        {
            // Update the paths in the area object
            update_dbus_paths(inv_bus_name, inv_obj_path, inv_intf_name);
        }
    }

#ifdef __IPMI_DEBUG__
            printf("fru_area=[%s], inv_bus_name=[%s], inv_obj_path=[%s], inv_intf_name=[%s]\n",
                    fru_area_name, inv_bus_name, inv_obj_path, inv_intf_name);
#endif

    sd_bus_error_free(&bus_error);
    sd_bus_message_unref(response);

    return rc;
}

//------------------------------------------------
// Takes the pointer to stream of bytes and length
// and returns the 8 bit checksum
// This algo is per IPMI V2.0 spec
//-------------------------------------------------
unsigned char calculate_crc(const unsigned char *data, size_t len)
{
    char crc = 0;
    size_t byte = 0;

    for(byte = 0; byte < len; byte++)
    {
        crc += *data++;
    }

    return(-crc);
}

//---------------------------------------------------------------------
// Accepts a fru area offset in commom hdr and tells which area it is.
//---------------------------------------------------------------------
ipmi_fru_area_type get_fru_area_type(uint8_t area_offset)
{
    ipmi_fru_area_type type = IPMI_FRU_AREA_TYPE_MAX;

    switch(area_offset)
    {
        case IPMI_FRU_INTERNAL_OFFSET:
            type = IPMI_FRU_AREA_INTERNAL_USE;
            break;

        case IPMI_FRU_CHASSIS_OFFSET:
            type = IPMI_FRU_AREA_CHASSIS_INFO;
            break;

        case IPMI_FRU_BOARD_OFFSET:
            type = IPMI_FRU_AREA_BOARD_INFO;
            break;

        case IPMI_FRU_PRODUCT_OFFSET:
            type = IPMI_FRU_AREA_PRODUCT_INFO;
            break;

        case IPMI_FRU_MULTI_OFFSET:
            type = IPMI_FRU_AREA_MULTI_RECORD;
            break;

        default:
            type = IPMI_FRU_AREA_TYPE_MAX;
    }

    return type;
}

///-----------------------------------------------
// Validates the data for crc and mandatory fields
///-----------------------------------------------
int verify_fru_data(const uint8_t *data, const size_t len)
{
    uint8_t checksum = 0;
    int rc = -1;

    // Validate for first byte to always have a value of [1]
    if(data[0] != IPMI_FRU_HDR_BYTE_ZERO)
    {
        fprintf(stderr, "Invalid entry:[%d] in byte-0\n",data[0]);
        return rc;
    }
#ifdef __IPMI_DEBUG__
    else
    {
        printf("SUCCESS: Validated [0x%X] in entry_1 of fru_data\n",data[0]);
    }
#endif

    // See if the calculated CRC matches with the embedded one.
    // CRC to be calculated on all except the last one that is CRC itself.
    checksum = calculate_crc(data, len - 1);
    if(checksum != data[len-1])
    {
#ifdef __IPMI_DEBUG__
        fprintf(stderr, "Checksum mismatch."
                " Calculated:[0x%X], Embedded:[0x%X]\n",
                checksum, data[len]);
#endif
        return rc;
    }
#ifdef __IPMI_DEBUG__
    else
    {
        printf("SUCCESS: Checksum matches:[0x%X]\n",checksum);
    }
#endif

    return EXIT_SUCCESS;
}

//------------------------------------------------------------------------
// Takes FRU data, invokes Parser for each fru record area and updates
// Inventory
//------------------------------------------------------------------------
int ipmi_update_inventory(fru_area_vec_t & area_vec)
{
    // Generic error reporter
    int rc = 0;

    // Dictionary object to hold Name:Value pair
    sd_bus_message *fru_dict = NULL;

    // SD Bus error report mechanism.
    sd_bus_error bus_error = SD_BUS_ERROR_NULL;

    // Response from sd bus calls
    sd_bus_message *response = NULL;

    // For each FRU area, extract the needed data , get it parsed and update
    // the Inventory.
    for(auto& iter : area_vec)
    {
        // Start fresh on each.
        sd_bus_error_free(&bus_error);
        sd_bus_message_unref(response);
        sd_bus_message_unref(fru_dict);

        // Constructor to allow further initializations and customization.
        rc = sd_bus_message_new_method_call((iter)->get_bus_type(),
                                            &fru_dict,
                                            (iter)->get_bus_name(),
                                            (iter)->get_obj_path(),
                                            (iter)->get_intf_name(),
                                            "update");
        if(rc < 0)
        {
            fprintf(stderr,"ERROR: creating a update method call for bus_name:[%s]\n",
                    (iter)->get_bus_name());
            break;
        }

        // A Dictionary ({}) having (string, variant)
        rc = sd_bus_message_open_container(fru_dict, 'a', "{sv}");
        if(rc < 0)
        {
            fprintf(stderr,"ERROR:[%d] creating a dict container:\n",errno);
            break;
        }

        // Fill the container with information
        rc = parse_fru_area((iter)->get_type(), (void *)(iter)->get_data(), (iter)->get_len(), fru_dict);
        if(rc < 0)
        {
            fprintf(stderr,"ERROR parsing FRU records\n");
            break;
        }

        sd_bus_message_close_container(fru_dict);

        // Now, Make the actual call to update the FRU inventory database with the
        // dictionary given by FRU Parser. There is no response message expected for
        // this.
        rc = sd_bus_call((iter)->get_bus_type(),     // On the System Bus
                         fru_dict,                   // With the Name:value dictionary array
                         0,                          //
                         &bus_error,                 // Object to return error.
                         &response);                 // Response message if any.

        if(rc < 0)
        {
            fprintf(stderr, "ERROR:[%s] updating FRU inventory for ID:[0x%X]\n",
                    bus_error.message, (iter)->get_fruid());
            break;
        }
        else if((iter)->is_bmc_fru())
        {
            // For FRUs that are accessible by HostBoot, host boot does all of
            // these.
            printf("SUCCESS: Updated:[%s_%d] successfully. Setting Valid bit\n",
                    (iter)->get_name(), (iter)->get_fruid());

            (iter)->set_valid(true);
        }
        else
        {
            printf("SUCCESS: Updated:[%s_%d] successfully\n",
                        (iter)->get_name(), (iter)->get_fruid());
        }
    } // END walking the vector of areas and updating

    sd_bus_error_free(&bus_error);
    sd_bus_message_unref(response);
    sd_bus_message_unref(fru_dict);

    return rc;
}

///----------------------------------------------------
// Checks if a particular fru area is populated or not
///----------------------------------------------------
bool remove_invalid_area(const std::unique_ptr<ipmi_fru> &fru_area)
{
    // Filter the ones that do not have dbus reference.
    if((strlen((fru_area)->get_bus_name()) == 0) ||
       (strlen((fru_area)->get_obj_path()) == 0)  ||
       (strlen((fru_area)->get_intf_name()) == 0))
    {
        return true;
    }
    return false;
}

///----------------------------------------------------------------------------------
// Populates various FRU areas
// @prereq : This must be called only after validating common header.
///----------------------------------------------------------------------------------
int ipmi_populate_fru_areas(uint8_t *fru_data, const size_t data_len,
                            fru_area_vec_t & fru_area_vec)
{
    size_t area_offset = 0;
    int rc = -1;

    // Now walk the common header and see if the file size has atleast the last
    // offset mentioned by the common_hdr. If the file size is less than the
    // offset of any if the fru areas mentioned in the common header, then we do
    // not have a complete file.
    for(uint8_t fru_entry = IPMI_FRU_INTERNAL_OFFSET;
            fru_entry < (sizeof(struct common_header) -2); fru_entry++)
    {
        rc = -1;
        // Actual offset in the payload is the offset mentioned in common header
        // multipled by 8. Common header is always the first 8 bytes.
        area_offset = fru_data[fru_entry] * IPMI_EIGHT_BYTES;
        if(area_offset && (data_len < (area_offset + 2)))
        {
            // Our file size is less than what it needs to be. +2 because we are
            // using area len that is at 2 byte off area_offset
            fprintf(stderr, "fru file is incomplete. Size:[%d]\n",data_len);
            return rc;
        }
        else if(area_offset)
        {
            // Read 2 bytes to know the actual size of area.
            uint8_t area_hdr[2] = {0};
            memcpy(area_hdr, &((uint8_t *)fru_data)[area_offset], sizeof(area_hdr));

            // Size of this area will be the 2nd byte in the fru area header.
            size_t  area_len = area_hdr[1] * IPMI_EIGHT_BYTES;
            uint8_t area_data[area_len] = {0};

            printf("fru data size:[%d], area offset:[%d], area_size:[%d]\n",
                    data_len, area_offset, area_len);

            // See if we really have that much buffer. We have area offset amd
            // from there, the actual len.
            if(data_len < (area_len + area_offset))
            {
                fprintf(stderr, "Incomplete Fru file.. Size:[%d]\n",data_len);
                return rc;
            }

            // Save off the data.
            memcpy(area_data, &((uint8_t *)fru_data)[area_offset], area_len);

            // Validate the crc
            rc = verify_fru_data(area_data, area_len);
            if(rc < 0)
            {
                fprintf(stderr, "Error validating fru area. offset:[%d]\n",area_offset);
                return rc;
            }
            else
            {
                printf("Successfully verified area checksum. offset:[%d]\n",area_offset);
            }

            // We already have a vector that is passed to us containing all
            // of the fields populated. Update the data portion now.
            for(auto& iter : fru_area_vec)
            {
                if((iter)->get_type() == get_fru_area_type(fru_entry))
                {
                    (iter)->set_data(area_data, area_len);
                }
            }
        } // If we have fru data present
    } // Walk common_hdr

    // Not all the fields will be populated in a fru data. Mostly all cases will
    // not have more than 2 or 3.
    fru_area_vec.erase(std::remove_if(fru_area_vec.begin(), fru_area_vec.end(),
                       remove_invalid_area), fru_area_vec.end());

    return EXIT_SUCCESS;
}

///---------------------------------------------------------
// Validates the fru data per ipmi common header constructs.
// Returns with updated common_hdr and also file_size
//----------------------------------------------------------
int ipmi_validate_common_hdr(const uint8_t *fru_data, const size_t data_len)
{
    int rc = -1;

    uint8_t common_hdr[sizeof(struct common_header)] = {0};
    if(data_len >= sizeof(common_hdr))
    {
        memcpy(common_hdr, fru_data, sizeof(common_hdr));
    }
    else
    {
        fprintf(stderr, "Incomplete fru data file. Size:[%d]\n", data_len);
        return rc;
    }

    // Verify the crc and size
    rc = verify_fru_data(common_hdr, sizeof(common_hdr));
    if(rc < 0)
    {
        fprintf(stderr, "Failed to validate common header\n");
        return rc;
    }

    return EXIT_SUCCESS;
}

//------------------------------------------------------------
// Cleanup routine
//------------------------------------------------------------
int cleanup_error(FILE *fru_fp, fru_area_vec_t & fru_area_vec)
{
    if(fru_fp != NULL)
    {
        fclose(fru_fp);
        fru_fp = NULL;
    }

    if(!(fru_area_vec.empty()))
    {
        fru_area_vec.clear();
    }

    return  -1;
}


///-----------------------------------------------------
// Get the fru area names defined in BMC for a given @fruid.
//----------------------------------------------------
int get_defined_fru_area(sd_bus *bus_type, const uint8_t fruid,
                         std::vector<std::string> &defined_fru_area)
{
    // Need this to get respective DBUS objects
    sd_bus_error bus_error = SD_BUS_ERROR_NULL;
    sd_bus_message *response = NULL;
    int rc = 0;
    char *areas = NULL;

#ifdef __IPMI_DEBUG__
    printf("Getting fru areas defined in Skeleton for :[%d]\n", fruid);
#endif

    // We want to call a method "getFRUArea" on System Bus that is
    // made available over OpenBmc system services.
    rc = sd_bus_call_method(bus_type,                   // On the System Bus
                            sys_bus_name,               // Service to contact
                            sys_object_name,            // Object path
                            sys_intf_name,              // Interface name
                            "getFRUArea",               // Method to be called
                            &bus_error,                 // object to return error
                            &response,                  // Response message on success
                            "y",                        // input message (integer)
                            fruid);                     // Argument

    if(rc < 0)
    {
        fprintf(stderr, "Failed to get fru area for fruid:[%d] to dbus: [%s]\n",
                    fruid, bus_error.message);
    }
    else
    {
        // if several fru area names are defined, the names are combined to
        // a string seperated by ','
        rc = sd_bus_message_read(response, "s", &areas);
        if(rc < 0)
        {
            fprintf(stderr, "Failed to parse response message from getFRUArea:[%s]\n",
                        strerror(-rc));
        }
        else
        {
#ifdef __IPMI_DEBUG__
            printf("get defined fru area: id: %d, areas: %s\n", fruid, areas);
#endif
            std::string area_name;
            std::stringstream ss(areas);
            // fru area names string is seperated by ',', parse it into tokens
            while (std::getline(ss, area_name, ','))
            {
                if (!area_name.empty())
                    defined_fru_area.emplace_back(area_name);
            }
        }
    }

    sd_bus_error_free(&bus_error);
    sd_bus_message_unref(response);

    return rc;
}


///-----------------------------------------------------
// Accepts the filename and validates per IPMI FRU spec
//----------------------------------------------------
int ipmi_validate_fru_area(const uint8_t fruid, const char *fru_file_name,
                           sd_bus *bus_type, const bool bmc_fru)
{
    size_t data_len = 0;
    size_t bytes_read = 0;
    int rc = -1;

    // Vector that holds individual IPMI FRU AREAs. Although MULTI and INTERNAL
    // are not used, keeping it here for completeness.
    fru_area_vec_t fru_area_vec;
    std::vector<std::string> defined_fru_area;

    // BMC defines fru areas that should be present in Skeleton
    rc = get_defined_fru_area(bus_type, fruid, defined_fru_area);
    if(rc < 0)
    {
        fprintf(stderr, "ERROR: cannot get defined fru area\n");
        return rc;
    }
    for(uint8_t fru_entry = IPMI_FRU_INTERNAL_OFFSET;
        fru_entry < (sizeof(struct common_header) -2); fru_entry++)
    {
        // Create an object and push onto a vector.
        std::unique_ptr<ipmi_fru> fru_area = std::make_unique<ipmi_fru>
                         (fruid, get_fru_area_type(fru_entry), bus_type, bmc_fru);

        // Physically being present
        bool present = std::ifstream(fru_file_name);
        fru_area->set_present(present);

        // Only setup dbus path for areas defined in BMC.
        // Otherwise Skeleton will report 'not found' error
        std::string fru_area_name = fru_area->get_name() + std::to_string(fruid);
        auto iter = std::find(defined_fru_area.begin(), defined_fru_area.end(),
                                  fru_area_name);
        if (iter != defined_fru_area.end())
        {
            fru_area->setup_sd_bus_paths();
        }
        fru_area_vec.emplace_back(std::move(fru_area));
    }

    FILE *fru_fp = fopen(fru_file_name,"rb");
    if(fru_fp == NULL)
    {
        fprintf(stderr, "ERROR: opening:[%s]\n",fru_file_name);
        perror("Error:");
        return cleanup_error(fru_fp, fru_area_vec);
    }

    // Get the size of the file to see if it meets minimum requirement
    if(fseek(fru_fp, 0, SEEK_END))
    {
        perror("Error:");
        return cleanup_error(fru_fp, fru_area_vec);
    }

    // Allocate a buffer to hold entire file content
    data_len = ftell(fru_fp);
    uint8_t fru_data[data_len] = {0};

    rewind(fru_fp);
    bytes_read = fread(fru_data, data_len, 1, fru_fp);
    if(bytes_read != 1)
    {
        fprintf(stderr, "Failed reading fru data. Bytes_read=[%d]\n",bytes_read);
        perror("Error:");
        return cleanup_error(fru_fp, fru_area_vec);
    }

    // We are done reading.
    fclose(fru_fp);
    fru_fp = NULL;

    rc = ipmi_validate_common_hdr(fru_data, data_len);
    if(rc < 0)
    {
        return cleanup_error(fru_fp, fru_area_vec);
    }

    // Now that we validated the common header, populate various fru sections if we have them here.
    rc = ipmi_populate_fru_areas(fru_data, data_len, fru_area_vec);
    if(rc < 0)
    {
        fprintf(stderr,"Populating FRU areas failed for:[%d]\n",fruid);
        return cleanup_error(fru_fp, fru_area_vec);
    }
    else
    {
        printf("SUCCESS: Populated FRU areas for:[%s]\n",fru_file_name);
    }

#ifdef __IPMI_DEBUG__
    for(auto& iter : fru_area_vec)
    {
        printf("FRU ID : [%d]\n",(iter)->get_fruid());
        printf("AREA NAME : [%s]\n",(iter)->get_name());
        printf("TYPE : [%d]\n",(iter)->get_type());
        printf("LEN : [%d]\n",(iter)->get_len());
        printf("BUS NAME : [%s]\n", (iter)->get_bus_name());
        printf("OBJ PATH : [%s]\n", (iter)->get_obj_path());
        printf("INTF NAME :[%s]\n", (iter)->get_intf_name());
    }
#endif

    // If the vector is populated with everything, then go ahead and update the
    // inventory.
    if(!(fru_area_vec.empty()))
    {

#ifdef __IPMI_DEBUG__
        printf("\n SIZE of vector is : [%d] \n",fru_area_vec.size());
#endif
        rc = ipmi_update_inventory(fru_area_vec);
        if(rc <0)
        {
            fprintf(stderr, "Error updating inventory\n");
        }
    }

    // we are done with all that we wanted to do. This will do the job of
    // calling any destructors too.
    fru_area_vec.clear();

    return rc;
}
