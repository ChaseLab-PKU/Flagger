/**
 * Copyright (C) 2016-2018 Xilinx, Inc
 * Author: Hem C Neema, Ryan Radjabi
 * Simple command line utility to inetract with SDX PCIe devices
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include <thread>
#include <chrono>
#include <curses.h>
#include <sstream>
#include <climits>
#include <regex>
#include <algorithm>
#include <getopt.h>
#include <sys/mman.h>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>

#include "xbutil.h"
#include "base.h"

#define FORMATTED_FW_DIR    "/opt/xilinx/firmware"
#define hex_digit "[0-9a-fA-F]+"

const size_t m2mBoSize = 256L * 1024 * 1024;
const size_t hostMemSize = 256L * 1024 * 1024;

static int bdf2index(std::string& bdfStr, unsigned& index)
{
    // Extract bdf from bdfStr.
    int dom = 0, b, d, f;
    char dummy;
    std::stringstream s(bdfStr);
    size_t n = std::count(bdfStr.begin(), bdfStr.end(), ':');
    if (n == 1)
        s >> std::hex >> b >> dummy >> d >> dummy >> f;
    else if (n == 2)
        s >> std::hex >> dom >> dummy >> b >> dummy >> d >> dummy >> f;
    if ((n != 1 && n != 2) || s.fail()) {
        std::cout << "ERROR: failed to extract BDF from " << bdfStr << std::endl;
        return -EINVAL;
    }

    for (unsigned i = 0; i < pcidev::get_dev_total(); i++) {
        auto dev = pcidev::get_dev(i);
        if (dom == dev->domain && b == dev->bus &&
            d == dev->dev && (f == 0 || f == 1)) {
            index = i;
            return 0;
        }
    }

    std::cout << "ERROR: No card found for " << bdfStr << std::endl;
    return -ENOENT;
}

static int str2index(const char *arg, unsigned& index)
{
    std::string devStr(arg);

    if (devStr.find(":") == std::string::npos) {
    // The arg contains a board index.
        unsigned long i;
        char *endptr;
        i = std::strtoul(arg, &endptr, 0);
        if (*endptr != '\0' || i >= UINT_MAX) {
            std::cout << "ERROR: " << devStr << " is not a valid card index."
                << std::endl;
            return -EINVAL;
        }
        index = i;
    } else {
    // The arg contains domain:bus:device.function string.
        int ret = bdf2index(devStr, index);
        if (ret != 0)
            return ret;
    }

    return 0;
}

static void print_pci_info(std::ostream &ostr)
{
    ostr << "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n";
    if (pcidev::get_dev_total() == 0) {
        ostr << "No card found!" << std::endl;
        return;
    }

    for (unsigned j = 0; j < pcidev::get_dev_total(); j++) {
        auto dev = pcidev::get_dev(j);
        if (dev->is_ready)
            ostr << " ";
        else
            ostr << "*";
        ostr << "[" << j << "] " << dev << std::endl;
    }

    if (pcidev::get_dev_total() != pcidev::get_dev_ready()) {
        ostr << "WARNING: card(s) marked by '*' are not ready." << std::endl;
    }
}

static int xrt_xbutil_version_cmp()
{
    /*check xbutil tools and xrt versions*/
    std::string xrt = "";
    try {
        xrt = sensor_tree::get<std::string>( "runtime.build.version", "N/A" ) + ","
        + sensor_tree::get<std::string>( "runtime.build.hash", "N/A" );
    } catch (std::exception const& e) {
        std::cout << e.what() << std::endl;
    }
    if ( xcldev::driver_version("xocl") != "unknown" &&
        xrt.compare(xcldev::driver_version("xocl") ) != 0 ) {
        std::cout << "\nERROR: Mixed versions of XRT and xbutil are not supported. \
            \nPlease install matching versions of XRT and xbutil or  \
            \ndefine env variable INTERNAL_BUILD to disable this check\n" << std::endl;
        return -1;
    }
    return 0;
}

inline bool getenv_or_null(const char* env)
{
    return getenv(env) ? true : false;
}

int main(int argc, char *argv[])
{
    unsigned index = 0xffffffff;
    unsigned regionIndex = 0xffffffff;
    unsigned computeIndex = 0xffffffff;
    unsigned short targetFreq[4] = {0, 0, 0, 0};
    unsigned fanSpeed = 0;
    unsigned long long startAddr = 0;
    unsigned int pattern_byte = 'J';//Rather than zero; writing char 'J' by default
    size_t sizeInBytes = 0;
    std::string outMemReadFile = "memread.out";
    std::string flashType = ""; // unset and empty by default
    std::string mcsFile1, mcsFile2;
    std::string xclbin;
    size_t blockSize = 0;
    int c;
    dd::ddArgs_t ddArgs;
    int result = 0;

    xcldev::baseInit();

    const char* exe = argv[ 0 ];
    if (argc == 1) {
        xcldev::printHelp(exe);
        return 1;
    }

    /* Make sure xrt version matches driver version except for "help"
     * and "version" subcommands. */
    if(std::strcmp(argv[1], "help" ) != 0 &&
        std::strcmp(argv[1], "version") != 0 &&
        std::strcmp(argv[1], "--version") != 0) {
        if (!getenv_or_null("INTERNAL_BUILD")) {
            if (xrt_xbutil_version_cmp() != 0)
                return -1;
        }
    }

    try {
    /*
     * Call xbmgmt flash if first argument is "flash". This calls
     * xbmgmt flash and never returns. All arguments will be passed
     * down to xbmgmt flash.
     */
    if(std::string( argv[ 1 ] ).compare( "flash" ) == 0) {
        std::cout << "WARNING: The xbutil sub-command flash has been deprecated. "
                  << "Please use the xbmgmt utility with flash sub-command for equivalent functionality.\n"
                  << std::endl;
        // get self path, launch xbmgmt from self path
        char buf[ PATH_MAX ] = {0};
        auto len = readlink( "/proc/self/exe", buf, PATH_MAX );
        if( len == -1 ) {
            perror( "readlink:" );
            return errno;
        }
        buf[ len - 1 ] = 0; // null terminate after successful read

        // remove exe name from this to get the parent path
        size_t found = std::string( buf ).find_last_of( "/\\" ); // finds the last backslash char
        std::string path = std::string( buf ).substr( 0, found );
        // coverity[TAINTED_STRING] argv will be validated inside xbmgmt flash
        // Let xbmgmt know that this call is from xbutil for backward
        // compatibility behavior in xbmgmt flash
        argv[1][0] = '-';
        return execv( std::string( path + "/xbmgmt" ).c_str(), argv );
    } /* end of call to xbmgmt flash */

    optind++;
    if( std::strcmp( argv[1], "validate" ) == 0 ) {
        return xcldev::xclValidate(argc, argv);
    } else if( std::strcmp( argv[1], "top" ) == 0 ) {
        return xcldev::xclTop(argc, argv);
    } else if( std::strcmp( argv[1], "reset" ) == 0 ) {
        return xcldev::xclReset(argc, argv);
    } else if( std::strcmp( argv[1], "p2p" ) == 0 ) {
        return xcldev::xclP2p(argc, argv);
    } else if( std::strcmp( argv[1], "host_mem" ) == 0 ) {
        return xcldev::xclCma(argc, argv);
    } else if( std::strcmp( argv[1], "scheduler" ) == 0 ) {
        return xcldev::xclScheduler(argc, argv);
    }
    optind--;

    argv++;
    const auto v = xcldev::commandTable.find(*argv);
    if (v == xcldev::commandTable.end()) {
        std::cout << "ERROR: Unknown comand \'" << *argv << "\'\n";
        xcldev::printHelp(exe);
        return 1;
    }

    const xcldev::command cmd = v->second;
    std::string cmdname = v->first;
    xcldev::subcommand subcmd = xcldev::MEM_READ;
    unsigned int ipmask = static_cast<unsigned int>(xcldev::STATUS_NONE_MASK);
    argc--;

    if (cmd == xcldev::HELP) {
        xcldev::printHelp(exe);
        return 0;
    }
    if (cmd == xcldev::VERSION) {
        xrt::version::print(std::cout);
        std::cout.width(26); std::cout << std::internal << "XOCL: " << sensor_tree::get<std::string>( "runtime.build.xocl", "N/A" )
                                       << std:: endl;
        std::cout.width(26); std::cout << std::internal << "XCLMGMT: " << sensor_tree::get<std::string>( "runtime.build.xclmgmt", "N/A" )
                                       << std::endl;
        return 0;
    }

    argv[0] = const_cast<char *>(exe);
    static struct option long_options[] = {
        {"read", no_argument, 0, xcldev::MEM_READ},
        {"write", no_argument, 0, xcldev::MEM_WRITE},
        {"aim", no_argument, 0, xcldev::STATUS_AIM},
        {"lapc", no_argument, 0, xcldev::STATUS_LAPC},
        {"asm", no_argument, 0, xcldev::STATUS_ASM},
        {"spc", no_argument, 0, xcldev::STATUS_SPC},
        {"tracefunnel", no_argument, 0, xcldev::STATUS_UNSUPPORTED},
        {"monitorfifolite", no_argument, 0, xcldev::STATUS_UNSUPPORTED},
        {"monitorfifofull", no_argument, 0, xcldev::STATUS_UNSUPPORTED},
        {"accelmonitor", no_argument, 0, xcldev::STATUS_AM},
        {"stream", no_argument, 0, xcldev::STREAM},
        {0, 0, 0, 0}
    };

    int long_index;
    const char* short_options = "a:b:c:d:e:f:g:h:i:o:p:r:s"; //don't add numbers
    while ((c = getopt_long(argc, argv, short_options, long_options, &long_index)) != -1)
    {
        if (cmd == xcldev::LIST) {
            std::cout << "ERROR: 'list' command does not accept any options\n";
            return -1;
        }
        switch (c)
        {
        //Deal with long options. Long options return the value set in option::val
        case xcldev::MEM_READ : {
            //--read
            if (cmd != xcldev::MEM) {
                std::cout << "ERROR: Option '" << long_options[long_index].name << "' cannot be used with command " << cmdname << "\n";
                return -1;
            }
            subcmd = xcldev::MEM_READ;
            break;
        }
        case xcldev::MEM_WRITE : {
            //--write
            if (cmd != xcldev::MEM) {
                std::cout << "ERROR: Option '" << long_options[long_index].name << "' cannot be used with command " << cmdname << "\n";
                return -1;
            }
            subcmd = xcldev::MEM_WRITE;
            break;
        }
        case xcldev::STATUS_LAPC : {
            //--lapc
            if (cmd != xcldev::STATUS) {
                std::cout << "ERROR: Option '" << long_options[long_index].name << "' cannot be used with command " << cmdname << "\n";
                return -1;
            }
            ipmask |= static_cast<unsigned int>(xcldev::STATUS_LAPC_MASK);
            break;
        }
        case xcldev::STATUS_AIM : {
            //--aim
            if (cmd != xcldev::STATUS) {
                std::cout << "ERROR: Option '" << long_options[long_index].name << "' cannot be used with command " << cmdname << "\n";
                return -1;
            }
            ipmask |= static_cast<unsigned int>(xcldev::STATUS_AIM_MASK);
            break;
        }
        case xcldev::STATUS_ASM : {
            if (cmd != xcldev::STATUS) {
                std::cout << "ERROR: Option '" << long_options[long_index].name << "' cannot be used with command " << cmdname << "\n" ;
                return -1 ;
            }
            ipmask |= static_cast<unsigned int>(xcldev::STATUS_ASM_MASK);
            break ;
        }
	case xcldev::STATUS_SPC: {
	  //--spc
	  if (cmd != xcldev::STATUS) {
	    std::cout << "ERROR: Option '" << long_options[long_index].name << "' cannot be used with command " << cmdname << "\n";
	    return -1;
	  }
	  ipmask |= static_cast<unsigned int>(xcldev::STATUS_SPC_MASK);
	  break;
	}
        case xcldev::STATUS_UNSUPPORTED : {
            //Don't give ERROR for as yet unsupported IPs
            std::cout << "INFO: No Status information available for IP: " << long_options[long_index].name << "\n";
            return 0;
        }
        case xcldev::STREAM:
        {
            if(cmd != xcldev::QUERY) {
                std::cout << "ERROR: Option '" << long_options[long_index].name << "' cannot be used with command " << cmdname << "\n";
                return -1;
            }
            subcmd = xcldev::STREAM;
            break;
        }
        case xcldev::STATUS_AM : {
            //--am
            if (cmd != xcldev::STATUS) {
                std::cout << "ERROR: Option '" << long_options[long_index].name << "' cannot be used with command " << cmdname << "\n";
                return -1;
            }
            ipmask |= static_cast<unsigned int>(xcldev::STATUS_AM_MASK);
            break;
        }
        //short options are dealt here
        case 'a':{
            if (cmd != xcldev::MEM) {
                std::cout << "ERROR: '-a' not applicable for this command\n";
                return -1;
            }
            size_t idx = 0;
            try {
                startAddr = std::stoll(optarg, &idx, 0);
            }
            catch (const std::exception& ex) {
                //out of range, invalid argument ex
                std::cout << "ERROR: Value supplied to -" << (char)c << " option is invalid\n";
                return -1;
            }
            if (idx < strlen(optarg)) {
                std::cout << "ERROR: Value supplied to -" << (char)c << " option is invalid\n";
                return -1;
            }
            break;
        }
        case 'o': {
            if (cmd != xcldev::MEM || subcmd != xcldev::MEM_READ) {
                std::cout << "ERROR: '-o' not applicable for this command\n";
                return -1;
            }
            outMemReadFile = optarg;
            break;
        }
        case 'e': {
            if (cmd != xcldev::MEM || subcmd != xcldev::MEM_WRITE) {
                std::cout << "ERROR: '-e' not applicable for this command\n";
                return -1;
            }
            size_t idx = 0;
            try {
                pattern_byte = std::stoi(optarg, &idx, 0);
            }
            catch (const std::exception& ex) {
                //out of range, invalid argument ex
                std::cout << "ERROR: Value supplied to -" << (char)c << " option must be a value between 0 and 255\n";
                return -1;
            }
            if (pattern_byte > 0xff || idx < strlen(optarg)) {
                std::cout << "ERROR: Value supplied to -" << (char)c << " option must be a value between 0 and 255\n";
                return -1;
            }
            break;
        }
        case 'i': {
            if (cmd != xcldev::MEM) {
                std::cout << "ERROR: '-i' not applicable for this command\n";
                return -1;
            }
            size_t idx = 0;
            try {
                sizeInBytes = std::stoll(optarg, &idx, 0);
            }
            catch (const std::exception& ex) {
                //out of range, invalid argument ex
                std::cout << "ERROR: Value supplied to -" << (char)c << " option is invalid\n";
                return -1;
            }
            if (idx < strlen(optarg)) {
                std::cout << "ERROR: Value supplied to -" << (char)c << " option is invalid\n";
                return -1;
            }
            break;
        }
        case 'd': {
            int ret = str2index(optarg, index);
            if (ret != 0)
                return ret;
            if (cmd == xcldev::DD) {
                ddArgs = dd::parse_dd_options( argc, argv );
            }
            break;
        }
        case 'r':
            if ((cmd == xcldev::BOOT) || (cmd == xcldev::DMATEST) ||(cmd == xcldev::STATUS)) {
                std::cout << "ERROR: '-r' not applicable for this command\n";
                return -1;
            }
            regionIndex = std::atoi(optarg);
            if((int)regionIndex < 0){
                std::cout << "ERROR: Region Index can not be " << (int)regionIndex << ", option is invalid\n";
                return -1;
            }
            break;
        case 'p':
            if (cmd != xcldev::PROGRAM) {
                std::cout << "ERROR: '-p' only allowed with 'program' command\n";
                return -1;
            }
            xclbin = optarg;
            break;
        case 'f':
            if (cmd != xcldev::CLOCK) {
                std::cout << "ERROR: '-f' only allowed with 'clock' command\n";
                return -1;
            }
            targetFreq[0] = std::atoi(optarg);
            break;
        case 'g':
            if (cmd != xcldev::CLOCK) {
                std::cout << "ERROR: '-g' only allowed with 'clock' command\n";
                return -1;
            }
            targetFreq[1] = std::atoi(optarg);
            break;
        case 'h':
            if (cmd != xcldev::CLOCK) {
                std::cout << "ERROR: '-h' only allowed with 'clock' command\n";
                return -1;
            }
            targetFreq[2] = std::atoi(optarg);
            break;
        case 'c':
            if (cmd != xcldev::RUN) {
                std::cout << "ERROR: '-c' only allowed with 'run' command\n";
                return -1;
            }
            computeIndex = std::atoi(optarg);
            break;
        case 's':
            if (cmd != xcldev::FAN) {
                std::cout << "ERROR: '-s' only allowed with 'fan' command\n";
                return -1;
            }
            fanSpeed = std::atoi(optarg);
            break;
        case 'b':
        {
            if (cmd != xcldev::DMATEST) {
                std::cout << "ERROR: '-b' only allowed with 'dmatest' command\n";
                return -1;
            }
            std::string tmp(optarg);
            if ((tmp[0] == '0') && (std::tolower(tmp[1]) == 'x')) {
                blockSize = std::stoll(tmp, 0, 16);
            }
            else {
                blockSize = std::stoll(tmp, 0, 10);
            }

            if (blockSize & (blockSize - 1)) {
                std::cout << "ERROR: block size should be power of 2\n";
                return -1;
            }

            if (blockSize > 0x100000) {
                std::cout << "ERROR: block size cannot be greater than 0x100000 KB\n";
                return -1;
            }
            blockSize *= 1024; // convert kilo bytes to bytes
            break;
        }
        default:
            xcldev::printHelp(exe);
            return 1;
        }
    }

    if (optind != argc) {
        std::cout << "ERROR: Illegal command \'" << argv[optind++] << "\'\n";
        return -1;
    }

    if (index == 0xffffffff) index = 0;

    if (regionIndex == 0xffffffff) regionIndex = 0;

    switch (cmd) {
    case xcldev::BOOT:
    case xcldev::RUN:
    case xcldev::FAN:
    case xcldev::DMATEST:
    case xcldev::MEM:
    case xcldev::QUERY:
    case xcldev::SCAN:
    case xcldev::STATUS:
    case xcldev::M2MTEST:
        break;
    case xcldev::PROGRAM:
    {
        if (xclbin.size() == 0) {
            std::cout << "ERROR: Please specify xclbin file with '-p' switch\n";
            return -1;
        }
        break;
    }
    case xcldev::CLOCK:
    {
        if (!targetFreq[0] && !targetFreq[1] && !targetFreq[2]) {
            std::cout << "ERROR: Please specify frequency(ies) with '-f' and or '-g' and or '-h' switch(es)\n";
            return -1;
        }
        break;
    }
    default:
        break;
    }

    std::vector<std::unique_ptr<xcldev::device>> deviceVec;

    unsigned int total = pcidev::get_dev_total();
    unsigned int count = pcidev::get_dev_ready();

    if (cmd != xcldev::DUMP)
        std::cout << "INFO: Found total " << total << " card(s), "
                  << count << " are usable" << std::endl;

    if (cmd == xcldev::LIST) {
        std::cout << "WARNING: The xbutil sub-command list has been deprecated. "
                  << "Please use xbutil scan for equivalent functionality.\n"
                  << std::endl;
    }
    if ((cmd == xcldev::QUERY) || (cmd == xcldev::SCAN) || (cmd == xcldev::LIST))
        xcldev::baseDump(std::cout);

    if (total == 0) {
        if (cmd == xcldev::DUMP)
            sensor_tree::json_dump( std::cout );
        // Querying a card with index 0 when it does not exist is an error
        if (cmd == xcldev::QUERY)
            return -ENODEV;
        // Enumerating cards when none exist is not an error
        if ((cmd == xcldev::SCAN) || (cmd == xcldev::LIST))
            return 0;
    }

    if (cmd == xcldev::SCAN || cmd == xcldev::LIST) {
        print_pci_info(std::cout);
        return 0;
    }

    for (unsigned i = 0; i < count; i++) {
        try {
            deviceVec.emplace_back(new xcldev::device(i, nullptr));
        } catch (const std::exception& ex) {
            std::cerr << ex.what() << std::endl;
	    return -ENODEV;
        }
    }

    if (index >= deviceVec.size()) {
        std::cout << "ERROR: Card index " << index << " is out of range";
        std::cout << std::endl;
        return -ENOENT;
    } else {
        if (index >= count) {
            std::cout << "ERROR: Card [" << index << "] is not ready";
            std::cout << std::endl;
            return -ENOENT;
        }
    }

    if (pcidev::get_dev(index) == NULL){
        std::cout << "ERROR: Card index " << index << " is not usable\n";
        return 1;
    }

    switch (cmd)
    {
    case xcldev::BOOT:
        result = deviceVec[index]->boot();
        break;
    case xcldev::CLOCK:
        result = deviceVec[index]->reclock2(regionIndex, targetFreq);
        break;
    case xcldev::FAN:
        result = deviceVec[index]->fan(fanSpeed);
        break;
    case xcldev::PROGRAM:
        result = deviceVec[index]->program(xclbin, regionIndex);
        break;
    case xcldev::QUERY:
        if(subcmd == xcldev::STREAM) {
            result = deviceVec[index]->printStreamInfo(std::cout);
        } else {
            result = deviceVec[index]->dump(std::cout);
        }
        break;
    case xcldev::DUMP:
        result = deviceVec[index]->dumpJson(std::cout);
        break;
    case xcldev::RUN:
        result = deviceVec[index]->run(regionIndex, computeIndex);
        break;
    case xcldev::DMATEST:
        result = deviceVec[index]->dmatest(blockSize, true);
        break;
    case xcldev::MEM:
        if (subcmd == xcldev::MEM_READ) {
            result = deviceVec[index]->memread(outMemReadFile, startAddr, sizeInBytes);
        } else if (subcmd == xcldev::MEM_WRITE) {
            result = deviceVec[index]->memwrite(startAddr, sizeInBytes, pattern_byte);
        }
        break;
    case xcldev::DD:
        result = deviceVec[index]->do_dd( ddArgs );
        break;
    case xcldev::STATUS:
        if (ipmask == xcldev::STATUS_NONE_MASK) {
            result = deviceVec[index]->print_debug_ip_list(0);
        }
        if (ipmask & static_cast<unsigned int>(xcldev::STATUS_LAPC_MASK)) {
            result = deviceVec[index]->readLAPCheckers(1);
        }
        if (ipmask & static_cast<unsigned int>(xcldev::STATUS_AIM_MASK)) {
            result = deviceVec[index]->readAIMCounters();
        }
        if (ipmask & static_cast<unsigned int>(xcldev::STATUS_ASM_MASK)) {
            result = deviceVec[index]->readASMCounters() ;
        }
        if (ipmask & static_cast<unsigned int>(xcldev::STATUS_AM_MASK)) {
            result = deviceVec[index]->readAMCounters();
        }
        if (ipmask & static_cast<unsigned int>(xcldev::STATUS_SPC_MASK)) {
	  result = deviceVec[index]->readStreamingCheckers(1);
	}
        break;
    case xcldev::M2MTEST:
        result = deviceVec[index]->testM2m();
        break;
    default:
        std::cout << "ERROR: Not implemented\n";
        result = -1;
    }

    if (result != 0)
        std::cout << "ERROR: xbutil " << v->first  << " failed." << std::endl;
    else if (cmd != xcldev::DUMP)
        std::cout << "INFO: xbutil " << v->first << " succeeded." << std::endl;
    } catch (std::exception& ex) {
      std::cout << ex.what() << std::endl;
      return -1;
    }

    return result;
}

void xcldev::printHelp(const std::string& exe)
{
    std::cout << "Running xbutil for 4.0+ shell's \n\n";
    std::cout << "Usage: " << exe << " <command> [options]\n\n";
    std::cout << "Command and option summary:\n";
    std::cout << "  clock   [-d card] [-r region] [-f clock1_freq_MHz] [-g clock2_freq_MHz] [-h clock3_freq_MHz]\n";
    std::cout << "  dmatest [-d card] [-b [0x]block_size_KB]\n";
    std::cout << "  dump\n";
    std::cout << "  help\n";
    std::cout << "  m2mtest [-d card]\n";
    std::cout << "  version\n";
    std::cout << "  mem --read [-d card] [-a [0x]start_addr] [-i size_bytes] [-o output filename]\n";
    std::cout << "  mem --write [-d card] [-a [0x]start_addr] [-i size_bytes] [-e pattern_byte]\n";
    std::cout << "  program [-d card] [-r region] -p xclbin\n";
    std::cout << "  query   [-d card [-r region]]\n";
    std::cout << "  status [-d card] [--debug_ip_name]\n";
    std::cout << "  scan\n";
    std::cout << "  top [-d card] [-i seconds]\n";
    std::cout << "  validate [-d card]\n";
    std::cout << "  reset  [-d card]\n";
    std::cout << " Requires root privileges:\n";
    std::cout << "  p2p    [-d card] --enable\n";
    std::cout << "  p2p    [-d card] --disable\n";
    std::cout << "  p2p    [-d card] --validate\n";
    std::cout << "  flash   [-d card] -m primary_mcs [-n secondary_mcs] [-o bpi|spi]\n";
    std::cout << "  flash   [-d card] -a <all | shell> [-t timestamp]\n";
    std::cout << "  flash   [-d card] -p msp432_firmware\n";
    std::cout << "  flash   scan [-v]\n";
    std::cout << "  host_mem   [-d card] --enable --[size sz M|G]\n";
    std::cout << "  host_mem   [-d card] --disable\n";
    std::cout << "\nNOTE: card for -d option can either be id or bdf\n";
    std::cout << "\nExamples:\n";
    std::cout << "Print JSON file to stdout\n";
    std::cout << "  " << exe << " dump\n";
    std::cout << "List all cards\n";
    std::cout << "  " << exe << " list\n";
    std::cout << "Scan for Xilinx PCIe card(s) & associated drivers (if any) and relevant system information\n";
    std::cout << "  " << exe << " scan\n";
    std::cout << "Change the clock frequency of region 0 in card 0 to 100 MHz\n";
    std::cout << "  " << exe << " clock -f 100\n";
    std::cout << "For card 0 which supports multiple clocks, change the clock 1 to 200MHz and clock 2 to 250MHz\n";
    std::cout << "  " << exe << " clock -f 200 -g 250\n";
    std::cout << "Download the accelerator program for card 2\n";
    std::cout << "  " << exe << " program -d 2 -p a.xclbin\n";
    std::cout << "Run DMA test on card 1 with 32 KB blocks of buffer\n";
    std::cout << "  " << exe << " dmatest -d 1 -b 0x20\n";
    std::cout << "Read 256 bytes from DDR/HBM/PLRAM starting at 0x1000 into file read.out\n";
    std::cout << "  " << exe << " mem --read -a 0x1000 -i 256 -o read.out\n";
    std::cout << "  " << "Default values for address is 0x0, size is DDR size and file is memread.out\n";
    std::cout << "Write 256 bytes to DDR/HBM/PLRAM starting at 0x1000 with byte 0xaa \n";
    std::cout << "  " << exe << " mem --write -a 0x1000 -i 256 -e 0xaa\n";
    std::cout << "  " << "Default values for address is 0x0, size is DDR size and pattern is 0x0\n";
    std::cout << "List the debug IPs available on the platform\n";
    std::cout << "  " << exe << " status \n";
    std::cout << "Validate installation on card 1\n";
    std::cout << "  " << exe << " validate -d 0000:02:00.0\n";
}

std::unique_ptr<xcldev::device> xcldev::xclGetDevice(unsigned index)
{
    try {
        unsigned int count = pcidev::get_dev_total();
        if (count == 0) {
            std::cout << "ERROR: No card found" << std::endl;
        } else if (index >= count) {
            std::cout << "ERROR: Card index " << index << " out of range";
            std::cout << std::endl;
        } else {
            return std::make_unique<xcldev::device>(index,nullptr);
        }
    }
    catch (const std::exception& ex) {
        std::cout << "ERROR: " << ex.what() << std::endl;
    }

    return nullptr;
}

struct topThreadCtrl {
    int interval;
    std::unique_ptr<xcldev::device> dev;
    bool quit;
    int status;
};

static void topPrintUsage(const xcldev::device *dev, xclDeviceUsage& devstat)
{
    std::vector<std::string> lines;

    dev->readSensors();

    dev->m_mem_usage_bar(devstat, lines);

    dev->sysfs_stringize_power(lines);

    dev->m_mem_usage_stringize_dynamics(devstat, lines);

    dev->m_stream_usage_stringize_dynamics(lines);

    dev->m_cu_usage_stringize_dynamics(lines);

    dev->clearSensorTree();
    for(auto line:lines) {
            printw("%s\n", line.c_str());
    }
}

static void topPrintStreamUsage(const xcldev::device *dev)
{
    std::vector<std::string> lines;

    dev->m_stream_usage_stringize_dynamics(lines);

    for(auto line:lines) {
        printw("%s\n", line.c_str());
    }

}

static void topThreadFunc(struct topThreadCtrl *ctrl)
{
    int i = 0;

    while (!ctrl->quit) {
        if ((i % ctrl->interval) == 0) {
            xclDeviceUsage devstat;
            int result = ctrl->dev->usageInfo(devstat);
            if (result) {
                ctrl->status = result;
                return;
            }
            clear();
            topPrintUsage(ctrl->dev.get(), devstat);
            refresh();
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        i++;
    }
}

static void topThreadStreamFunc(struct topThreadCtrl *ctrl)
{
    int i = 0;

    while (!ctrl->quit) {
        if ((i % ctrl->interval) == 0) {
            xclDeviceUsage devstat;
            int result = ctrl->dev->usageInfo(devstat);
            if (result) {
                ctrl->status = result;
                return;
            }
            clear();
            topPrintStreamUsage(ctrl->dev.get());
            refresh();
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        i++;
    }
}

void xclTopHelp()
{
    std::cout << "Options: [-d <card>]: device index\n";
    std::cout << "         [-i <interval>]: refresh interval\n";
    std::cout << "         [-s]: display stream topology \n";
}

int xcldev::xclTop(int argc, char *argv[])
{
    int interval = 1;
    unsigned index = 0;
    int c;
    bool printStreamOnly = false;
    struct topThreadCtrl ctrl = { 0 };

    while ((c = getopt(argc, argv, "d:i:s")) != -1) {
        switch (c) {
        case 'i':
            interval = std::atoi(optarg);
            if (interval < 1)
                interval = 1;
            break;
        case 'd': {
            int ret = str2index(optarg, index);
            if (ret != 0)
                return ret;
            break;
        }
        case 's':
            printStreamOnly = true;
            break;
        default:
            xclTopHelp();
            return -EINVAL;
        }
    }

    if (optind != argc) {
        xclTopHelp();
        return -EINVAL;
    }

    ctrl.interval = interval;

    ctrl.dev = xcldev::xclGetDevice(index);
    if (!ctrl.dev) {
        return -ENOENT;
    }

    initscr();
    cbreak();
    noecho();
    std::thread t;
    if (printStreamOnly) {
        t = std::thread(topThreadStreamFunc, &ctrl);
    } else {
        t = std::thread(topThreadFunc, &ctrl);
    }


    // Waiting for and processing control command from stdin
    while (!ctrl.quit) {
        switch (getch()) {
        case 'q':
        case ERR:
            ctrl.quit = true;
            break;
        default:
            break;
        }
    }

    t.join();
    endwin();
    return ctrl.status;
}

const std::string dsaPath("/opt/xilinx/dsa/");
const std::string xrtPath("/opt/xilinx/xrt/");

std::string getXsaPath(const uint16_t vendor)
{
    if ((vendor == 0) || (vendor == INVALID_ID))
        return std::string();

    std::string vendorName;
    switch (vendor) {
        case ARISTA_ID:
            vendorName = "arista";
            break;
        default:
        case XILINX_ID:
            vendorName = "xilinx";
            break;
    }
    return "/opt/" + vendorName + "/xsa/";
}

void testCaseProgressReporter(bool *quit)
{    int i = 0;
    while (!*quit) {
        if (i != 0 && (i % 5 == 0))
            std::cout << "." << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        i++;
    }
}

inline const char* getenv_or_empty(const char* path)
{
    return getenv(path) ? getenv(path) : "";
}

static void set_shell_path_env(const std::string& var_name,
    const std::string& trailing_path)
{
    std::string xrt_path(getenv_or_empty("XILINX_XRT"));
    std::string new_path(getenv_or_empty(var_name.c_str()));
    xrt_path += trailing_path + ":";
    new_path = xrt_path + new_path;
    setenv(var_name.c_str(), new_path.c_str(), 1);
}

int runShellCmd(const std::string& cmd, std::string& output)
{
    int ret = 0;
    bool quit = false;

    // Fix environment variables before running test case
    setenv("XILINX_XRT", "/opt/xilinx/xrt", 0);
    set_shell_path_env("PYTHONPATH", "/python");
    set_shell_path_env("LD_LIBRARY_PATH", "/lib");
    set_shell_path_env("PATH", "/bin");
    unsetenv("XCL_EMULATION_MODE");

    int stderr_fds[2];
    if (pipe(stderr_fds)== -1) {
        perror("ERROR: Unable to create pipe");
        return -errno;
    }

    // Save stderr
    int stderr_save = dup(STDERR_FILENO);
    if (stderr_save == -1) {
        perror("ERROR: Unable to duplicate stderr");
        return -errno;
    }

    // Kick off progress reporter
    std::thread t(testCaseProgressReporter, &quit);

    // Close existing stderr and set it to be the write end of the pipe.
    // After fork below, our child process's stderr will point to the same fd.
    dup2(stderr_fds[1], STDERR_FILENO);
    close(stderr_fds[1]);
    std::shared_ptr<FILE> stderr_child(fdopen(stderr_fds[0], "r"), fclose);
    std::shared_ptr<FILE> stdout_child(popen(cmd.c_str(), "r"), pclose);
    // Restore our normal stderr
    dup2(stderr_save, STDERR_FILENO);
    close(stderr_save);

    if (stdout_child == nullptr) {
        std::cout << "ERROR: Failed to run " << cmd << std::endl;
        ret = -EINVAL;
    }

    // Read child's stdout and stderr without parsing the content
    char buf[1024];
    while (ret == 0 && !feof(stdout_child.get())) {
        if (fgets(buf, sizeof (buf), stdout_child.get()) != nullptr) {
            output += buf;
        }
    }
    while (ret == 0 && stderr_child && !feof(stderr_child.get())) {
        if (fgets(buf, sizeof (buf), stderr_child.get()) != nullptr) {
            output += buf;
        }
    }

    // Stop progress reporter
    quit = true;
    t.join();

    return ret;
}

int searchXsaAndDsa(int index, std::string xsaPath, std::string
    dsaPath, std::string& path, std::string &output)
{
    struct stat st;
    if (stat(xsaPath.c_str(), &st) == 0) {
        path =  xsaPath;
        return EXIT_SUCCESS;
    } else if (stat(dsaPath.c_str(), &st) == 0) {
        path = dsaPath;
        return EXIT_SUCCESS;
    }
    // Check if it is 2rp platform
    std::string logic_uuid;
    std::string errmsg;
    pcidev::get_dev(index)->sysfs_get( "", "logic_uuids", errmsg, logic_uuid);
    if (!logic_uuid.empty()) {
        DIR *dp;

	dp = opendir(FORMATTED_FW_DIR);
	if (!dp) {
            output += "ERROR: Failed to find firmware installation dir ";
	    output += FORMATTED_FW_DIR;
	    output += "\n";
	    return -ENOENT;
	}
	closedir(dp);

        boost::filesystem::path formatted_fw_dir(FORMATTED_FW_DIR);
        std::vector<std::string> suffix = { "dsabin", "xsabin" };
        for (std::string t : suffix) {
            std::regex e("(^" FORMATTED_FW_DIR "/[^/]+/[^/]+/[^/]+/).+\\." + t);
            for (boost::filesystem::recursive_directory_iterator iter(formatted_fw_dir, boost::filesystem::symlink_option::recurse), end;
                iter != end;
            )
            {
                std::string name = iter->path().string();
                std::cmatch cm;

                dp = opendir(name.c_str());
                if (!dp)
                {
                    iter.no_push();
                }
                else
                {
                    iter.no_push(false);
                    closedir(dp);
                }

                std::regex_match(name.c_str(), cm, e);
                if (cm.size() > 0)
                {
                    std::shared_ptr<char> dtbbuf = nullptr;
                    std::vector<std::string> uuids;
		    pcidev::get_axlf_section(name, PARTITION_METADATA, dtbbuf);
		    if (dtbbuf == nullptr)
                    {
                        ++iter;
                        continue;
		    }
		    pcidev::get_uuids(dtbbuf, uuids);
                    if (!uuids.size())
                    {
                        ++iter;
                        continue;
		    }
                    if (uuids[0].compare(logic_uuid) == 0)
                    {
                        path = cm.str(1) + "test/";
                        return EXIT_SUCCESS;
                    }
                }
                else if (iter.level() > 4)
                {
                    iter.pop();
                    continue;
                }
		++iter;
            }
        }
        output += "ERROR: Failed to find xclbin in ";
        output += FORMATTED_FW_DIR;
        output += "\n";
        return -ENOENT;
    }

    output += "ERROR: Failed to find xclbin in ";
    output += xsaPath;
    output += " and ";
    output += dsaPath;
    return -ENOENT;
}

int xcldev::device::runTestCase(const std::string& py,
    const std::string& xclbin, std::string& output, const std::string &args = "")
{
    struct stat st;

    std::string name, errmsg;
    pcidev::get_dev(m_idx)->sysfs_get( "rom", "VBNV", errmsg, name );
    if (!errmsg.empty()) {
        std::cout << errmsg << std::endl;
        return -EINVAL;
    }

    uint16_t vendor;
    pcidev::get_dev(m_idx)->sysfs_get<uint16_t>("", "vendor", errmsg, vendor, -1);
    if (!errmsg.empty()) {
        std::cout << errmsg << std::endl;
        return -EINVAL;
    }

    std::string devInfoPath = name + "/test/";
    std::string xsaXclbinPath = getXsaPath(vendor) + devInfoPath;
    std::string dsaXclbinPath = dsaPath + devInfoPath;
    std::string xrtTestCasePath = xrtPath + "test/";

    output.clear();

    std::string xclbinPath;
    searchXsaAndDsa(m_idx, xsaXclbinPath, dsaXclbinPath, xclbinPath, output);
    std::string cmd;

    // if platform.json exists in the new shell pakcage then use the new testcase
    // else fallback to old python test cases

    // NEW FLOW: runs if platform.json is available
    auto json_exists = [xclbinPath]() { return boost::filesystem::exists(xclbinPath + "platform.json") ? true : false; };

    if(json_exists()) {
        //map old testcase names to new testcase names
        static const std::map<std::string, std::string> test_map = {
            { "22_verify.py",             "validate.exe"    },
            { "23_bandwidth.py",          "kernel_bw.exe"   },
            { "host_mem_23_bandwidth.py", "slavebridge.exe" },
            { "xrt_iops_test.exe",        "xrt_iops_test.exe" },
            { "xcl_iops_test.exe",        "xcl_iops_test.exe" }
        };

        if (test_map.find(py) == test_map.end())
            return -EOPNOTSUPP;

        xrtTestCasePath += test_map.find(py)->second;
        // in case the user is trying to run a new platform with an XRT which doesn't support the new platform pkg
        if (!boost::filesystem::exists(xrtTestCasePath)) {
            output += "ERROR: Failed to find " + xrtTestCasePath + ", Shell package not installed properly.";
            return -ENOENT;
        }

        //get bdf from index
        auto device = pcidev::get_dev(m_idx);
        std::string bdf = boost::str(boost::format("%04x:%02x:%02x.%01x") % device->domain % device->bus % device->dev % device->func);

        cmd = xrtTestCasePath + " -p " + xclbinPath + " -d " + bdf + " " + args;

    }
    else if (py.find(".exe") == std::string::npos) { //OLD FLOW:
        // Use suffix ".exe" to identify special test case is not ideal. Let's do it for now.
        // We could refine this once need.
        xrtTestCasePath += py;
        xclbinPath += xclbin;

        if (stat(xrtTestCasePath.c_str(), &st) != 0 || stat(xclbinPath.c_str(), &st) != 0) {
            // 0RP (nonDFX) flat shell support.
            // Currently, there isn't a clean way to determine if a nonDFX shell's interface is truly flat.
            // At this time, this is determined by whether or not it delivers an accelerator (e.g., verify.xclbin)
            std::string logic_uuid, errmsg;
            pcidev::get_dev(m_idx)->sysfs_get( "", "logic_uuids", errmsg, logic_uuid);

            // Only skip the test if it nonDFX platform and the accelerator doesn't exist.
            // All other conditions should generate an error.
            if (!logic_uuid.empty() && xclbin.compare("verify.xclbin") == 0) {
                output += "Verify xclbin not available or shell partition is not programmed. Skipping validation.";
                return -EOPNOTSUPP;
            }
            //if bandwidth xclbin isn't present, skip the test
            if(xclbin.compare("bandwidth.xclbin") == 0) {
                output += "Bandwidth xclbin not available. Skipping validation.";
                return -EOPNOTSUPP;
            }
            output += "ERROR: Failed to find ";
            output += py;
            output += " or ";
            output += xclbin;
            output += ", Shell package not installed properly.";
            return -ENOENT;
        }

        // Program xclbin first.
        int ret = program(xclbinPath, 0);
        if (ret != 0) {
            output += "ERROR: Failed to download xclbin: ";
            output += xclbin;
            return -EINVAL;
        }

        cmd = "/usr/bin/python3 " + xrtTestCasePath + " -k " + xclbinPath + " -d " + std::to_string(m_idx);
    }
    else {
        xrtTestCasePath += py;
        xclbinPath += xclbin;

        cmd = xrtTestCasePath + " -k " + xclbinPath + " -d " + std::to_string(m_idx) + args;
    }
    return runShellCmd(cmd, output);
}

int xcldev::device::verifyKernelTest(void)
{
    std::string output;
    int ret = runTestCase(std::string("22_verify.py"),
        std::string("verify.xclbin"), output);

    if (ret != 0) {
        std::cout << output << std::endl;
        return ret;
    }

    if (output.find("Hello World") == std::string::npos) {
        std::cout << output << std::endl;
        ret = -EINVAL;
    }
    return ret;
}

int xcldev::device::bandwidthKernelTest(void)
{
    std::string output;

    //versal bandwidth kernel is different, hence it needs to run a custom testcase
    std::string errmsg, vbnv;
    pcidev::get_dev(m_idx)->sysfs_get("rom", "VBNV", errmsg, vbnv);

    std::string testcase = (vbnv.find("vck5000") != std::string::npos)
        ? "versal_23_bandwidth.py" : "23_bandwidth.py";

    int ret = runTestCase(testcase, std::string("bandwidth.xclbin"), output);

    if (ret != 0) {
        std::cout << output << std::endl;
        return ret;
    }

    if (output.find("PASS") == std::string::npos) {
        std::cout << output << std::endl;
        return -EINVAL;
    }

    // Print out max thruput
    size_t st = output.find("Maximum");
    if (st != std::string::npos) {
        size_t end = output.find("\n", st);
        std::cout << std::endl << output.substr(st, end - st) << std::endl;
    }

    return 0;
}

int xcldev::device::hostMemBandwidthKernelTest(void)
{
    std::string output;

    //Kick start hostMemBandwidthKernelTest only if enabled
    std::string errmsg;
    uint64_t host_mem_size = 0;
    pcidev::get_dev(m_idx)->sysfs_get<uint64_t>("address_translator", "host_mem_size",  errmsg, host_mem_size, 0);

    if (!host_mem_size) {
        std::cout << "Host_mem is not available. Skipping validation" << std::endl;
        return -EOPNOTSUPP;
    }

    std::string testcase = "host_mem_23_bandwidth.py";

    int ret = runTestCase(testcase, std::string("bandwidth.xclbin"), output);

    if (ret != 0) {
        std::cout << output << std::endl;
        return ret;
    }

    if (output.find("PASS") == std::string::npos) {
        std::cout << output << std::endl;
        return -EINVAL;
    }

    // Print out max thruput
    size_t st = output.find("Maximum");
    if (st != std::string::npos) {
        size_t end = output.find("\n", st);
        std::cout << std::endl << output.substr(st, end - st) << std::endl;
    }

    return 0;
}

int xcldev::device::scVersionTest(void)
{
    std::string sc_ver, exp_sc_ver;
    std::string errmsg;

    pcidev::get_dev(m_idx)->sysfs_get("xmc", "bmc_ver", errmsg, sc_ver);
    if (!errmsg.empty()) {
        std::cout << errmsg << std::endl;
        return -EINVAL;
    }

    pcidev::get_dev(m_idx)->sysfs_get("xmc", "exp_bmc_ver", errmsg, exp_sc_ver);
    if (!errmsg.empty()) {
        std::cout << errmsg << std::endl;
        return -EINVAL;
    }

    if (!exp_sc_ver.empty() && (sc_ver.compare(exp_sc_ver) != 0 || sc_ver.empty())) {
        std::cout << "SC FIRMWARE MISMATCH, ATTENTION" << std::endl;
        if (sc_ver.empty())
            std::cout << "Can't determine SC firmware running on board.";
        else if (sc_ver.compare(exp_sc_ver) != 0)
            std::cout << "SC firmware running on board: " << sc_ver;
        std::cout << ". Expected SC firmware from installed Shell: " << exp_sc_ver << std::endl;

        std::cout << "Please use \"xbmgmt flash --scan\" to check installed Shell." << std::endl;
        return 1;
    }

    return 0;
}

int xcldev::device::pcieLinkTest(void)
{
    unsigned int pcie_speed, pcie_speed_max, pcie_width, pcie_width_max;
    std::string errmsg;

    if (!errmsg.empty()) {
        std::cout << errmsg << std::endl;
        return -EINVAL;
    }

    pcidev::get_dev(m_idx)->sysfs_get<unsigned int>( "", "link_speed",     errmsg, pcie_speed, -1 );
    pcidev::get_dev(m_idx)->sysfs_get<unsigned int>( "", "link_speed_max", errmsg, pcie_speed_max, -1 );
    pcidev::get_dev(m_idx)->sysfs_get<unsigned int>( "", "link_width",     errmsg, pcie_width, -1 );
    pcidev::get_dev(m_idx)->sysfs_get<unsigned int>( "", "link_width_max", errmsg, pcie_width_max, -1 );
    if (pcie_speed != pcie_speed_max || pcie_width != pcie_width_max) {
        std::cout << "LINK ACTIVE, ATTENTION" << std::endl;
        std::cout << "Ensure Card is plugged in to Gen"
            << pcie_speed_max << "x" << pcie_width_max << ", instead of Gen"
            << pcie_speed << "x" << pcie_width << std::endl
            << "Lower performance may be experienced" << std::endl;
        return 1;
    }
    return 0;
}

int xcldev::device::auxConnectionTest(void)
{
    std::string name, errmsg;
    unsigned short max_power = 0;
    std::vector<std::string> auxPwrRequiredBoard =
        { "VCU1525", "U200", "U250", "U280" };
    bool auxBoard = false;

    if (!errmsg.empty()) {
        std::cout << errmsg << std::endl;
        return -EINVAL;
    }

    pcidev::get_dev(m_idx)->sysfs_get("xmc", "bd_name", errmsg, name);
    pcidev::get_dev(m_idx)->sysfs_get<unsigned short>("xmc", "max_power",  errmsg, max_power, 0);

    if (!name.empty()) {
        for (auto bd : auxPwrRequiredBoard) {
            if (name.find(bd) != std::string::npos) {
                auxBoard = true;
                break;
            }
        }
    }

    if (!auxBoard) {
        std::cout << "AUX power connector not available. Skipping validation"
                  << std::endl;
        return -EOPNOTSUPP;
    }

    //check aux cable if board u200, u250, u280
    if(max_power == 0) {
        std::cout << "AUX POWER NOT CONNECTED, ATTENTION" << std::endl;
        std::cout << "Board not stable for heavy acceleration tasks." << std::endl;
        return 1;
    }
    return 0;
}

int xcldev::device::powerTest(void)
{
    std::string name, errmsg;
    int power_warn = 0;

    if (!errmsg.empty()) {
        std::cout << errmsg << std::endl;
        return -EINVAL;
    }

    pcidev::get_dev(m_idx)->sysfs_get<int>("xmc", "xmc_power_warn",  errmsg, power_warn, 0);

    if(power_warn == 1) {
        std::cout << "POWER WARNING IS ON, ATTENTION" << std::endl;
        std::cout << "Sensor data might not be valid" << std::endl;
        return 1;
    }
    return 0;
}

int xcldev::device::runOneTest(std::string testName,
    std::function<int(void)> testFunc)
{
    std::cout << "INFO: == Starting " << testName << ": " << std::endl;

    int ret = testFunc();

    if (ret == 0) {
	    std::cout << "INFO: == " << testName << " PASSED" << std::endl;
    } else if (ret == -EOPNOTSUPP) {
	    std::cout << "INFO: == " << testName << " SKIPPED" << std::endl;
        if(testName.compare("verify kernel test") != 0)
            ret = 0;
    } else if (ret == 1) {
	    std::cout << "WARN: == " << testName << " PASSED with warning"
            << std::endl;
    } else {
        std::cout << "ERROR: == " << testName << " FAILED" << std::endl;
    }
    return ret;
}

int xcldev::device::getXclbinuuid(uuid_t &uuid) {
    std::string errmsg, xclbinid;

    pcidev::get_dev(m_idx)->sysfs_get("", "xclbinuuid", errmsg, xclbinid);

    if (!errmsg.empty()) {
        std::cout<<errmsg<<std::endl;
        return -ENODEV;
    }

    uuid_parse(xclbinid.c_str(), uuid);

    if (uuid_is_null(uuid)) {
        std::cout<<"  WARNING: 'uuid' invalid, unable to find uuid. \n"
                << "  Has the bitstream been loaded? See 'xbutil program'.\n";
        return -ENODEV;
    }

    return 0;
}

/*
 * validate
 */
int xcldev::device::validate(bool quick, bool hidden)
{
    bool withWarning = false;
    int retVal = 0;
    auto dev = pcidev::get_dev(m_idx);

    retVal = runOneTest("AUX power connector check",
            std::bind(&xcldev::device::auxConnectionTest, this));
    withWarning = withWarning || (retVal == 1);
    if (retVal < 0)
        return retVal;

    retVal = runOneTest("Power warning check",
            std::bind(&xcldev::device::powerTest, this));
    withWarning = withWarning || (retVal == 1);
    if (retVal < 0)
        return retVal;

    // Check pcie training
    retVal = runOneTest("PCIE link check",
            std::bind(&xcldev::device::pcieLinkTest, this));
    withWarning = withWarning || (retVal == 1);
    if (retVal < 0)
        return retVal;

    // Check SC firmware version
    retVal = runOneTest("SC firmware version check",
            std::bind(&xcldev::device::scVersionTest, this));
    withWarning = withWarning || (retVal == 1);
    if (retVal < 0)
        return retVal;

    // Test verify kernel
    retVal = runOneTest("verify kernel test",
            std::bind(&xcldev::device::verifyKernelTest, this));
    withWarning = withWarning || (retVal == 1);
    //flat shell support: if the shell doesn't support xclbin download
    //exit immediately
    if(retVal == -EOPNOTSUPP)
        return withWarning ? 1 : 0;
    if (retVal < 0)
        return retVal;

    // Skip the rest of test cases for quicker turn around.
    if (quick)
        return withWarning ? 1 : 0;

    // Perform IOPS test
    retVal = runOneTest("IOPS test",
            std::bind(&xcldev::device::iopsTest, this));
    withWarning = withWarning || (retVal == 1);
    if (retVal < 0)
        return retVal;

    // Perform DMA test
    retVal = runOneTest("DMA test",
            std::bind(&xcldev::device::dmatest, this, 0, false));
    withWarning = withWarning || (retVal == 1);
    if (retVal < 0)
        return retVal;

    // Test bandwidth kernel
    retVal = runOneTest("device memory bandwidth test",
            std::bind(&xcldev::device::bandwidthKernelTest, this));
    withWarning = withWarning || (retVal == 1);
    if (retVal < 0)
        return retVal;

    if (dev != nullptr) {
        std::string errmsg;
	int nodma = 0;

        dev->sysfs_get<int>("", "nodma", errmsg, nodma, 0);
        if (nodma) {
            retVal = runOneTest("host memory bandwidth test",
                std::bind(&xcldev::device::hostMemBandwidthKernelTest, this));
            withWarning = withWarning || (retVal == 1);
            if (retVal < 0)
                return retVal;

            return withWarning ? 1 : 0;
        }
    }

    // Perform P2P test
    retVal = runOneTest("PCIE peer-to-peer test",
            std::bind(&xcldev::device::testP2p, this));
    withWarning = withWarning || (retVal == 1);
    if (retVal < 0)
        return retVal;

    //Perform M2M test
    retVal = runOneTest("memory-to-memory DMA test",
            std::bind(&xcldev::device::testM2m, this));
    withWarning = withWarning || (retVal == 1);
    if (retVal < 0)
        return retVal;

    retVal = runOneTest("host memory bandwidth test",
            std::bind(&xcldev::device::hostMemBandwidthKernelTest, this));
    withWarning = withWarning || (retVal == 1);
    if (retVal < 0)
        return retVal;

    return withWarning ? 1 : 0;
}

int xcldev::xclValidate(int argc, char *argv[])
{
    unsigned index = UINT_MAX;
    const std::string usage("Options: [-d index]");
    int c;
    bool quick = false;
    bool hidden = false;

    while ((c = getopt(argc, argv, "d:qh")) != -1) {
        switch (c) {
        case 'd': {
            int ret = str2index(optarg, index);
            if (ret != 0)
                return ret;
            break;
        }
        case 'q':
            quick = true;
            break;
        case 'h':
            hidden = true;
            break;
        default:
            std::cerr << usage << std::endl;
            return -EINVAL;
        }
    }
    if (optind != argc) {
        std::cerr << usage << std::endl;
        return -EINVAL;
    }

    unsigned int count = pcidev::get_dev_total();

    std::vector<unsigned> boards;
    if (index == UINT_MAX) {
        if (count == 0) {
            std::cout << "ERROR: No card found" << std::endl;
            return -ENOENT;
        }
        for (unsigned i = 0; i < count; i++)
            boards.push_back(i);
    } else {
        if (index >= count) {
            std::cout << "ERROR: Card[" << index << "] not found" << std::endl;
            return -ENOENT;
        }
        boards.push_back(index);
    }

    std::cout << "INFO: Found " << boards.size() << " cards" << std::endl;

    bool warning = false;
    bool validated = true;
    for (unsigned i : boards) {
        std::unique_ptr<device> dev = xclGetDevice(i);
        if (!dev) {
            std::cout << "ERROR: Can't open card[" << i << "]" << std::endl;
            validated = false;
            continue;
        }

        std::cout << std::endl << "INFO: Validating card[" << i << "]: "
            << dev->name() << std::endl;

        int v = dev->validate(quick, hidden);
        if (v == 1) {
            warning = true;
            std::cout << "INFO: Card[" << i << "] validated with warnings." << std::endl;
        } else if (v != 0) {
            validated = false;
            std::cout << "INFO: Card[" << i << "] failed to validate." << std::endl;
        } else {
            std::cout << "INFO: Card[" << i << "] validated successfully." << std::endl;
        }
    }
    std::cout << std::endl;

    if (!validated) {
        std::cout << "ERROR: Some cards failed to validate." << std::endl;
        return -EINVAL;
    }

    if(warning)
        std::cout << "INFO: All cards validated successfully but with warnings." << std::endl;
    else
        std::cout << "INFO: All cards validated successfully." << std::endl;

    return 0;
}

int xcldev::device::reset(xclResetKind kind)
{
#ifdef __GNUC__
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    return xclResetDevice(m_handle, kind);
#ifdef __GNUC__
# pragma GCC diagnostic pop
#endif

}

bool canProceed()
{
    std::string input;
    bool answered = false;
    bool proceed = false;

    while (!answered) {
        std::cout << "Are you sure you wish to proceed? [y/n]: ";
        std::cin >> input;
        if(input.compare("y") == 0 || input.compare("n") == 0)
            answered = true;
    }

    proceed = (input.compare("y") == 0);
    if (!proceed)
        std::cout << "Action canceled." << std::endl;
    return proceed;
}

static int isSudo()
{
    if ((getuid() == 0) || (geteuid() == 0))
        return 0;
    std::cerr << "ERROR: root privileges required." << std::endl;
    return -EPERM;
}

int xcldev::xclReset(int argc, char *argv[])
{
    int c;
    unsigned index = 0;
    const std::string usage("Options: [-d index]");

    while ((c = getopt(argc, argv, "d:")) != -1) {
        switch (c) {
        case 'd': {
            int ret = str2index(optarg, index);
            if (ret != 0)
                return ret;
            break;
        }
        default:
            std::cerr << usage << std::endl;
            return -EINVAL;
        }
    }
    if (optind != argc) {
        std::cerr << usage << std::endl;
        return -EINVAL;
    }

    if (index >= pcidev::get_dev_total()) {
        std::cout << "ERROR: index " << index << " out of range" << std::endl;
        return -EINVAL;
    }

    std::string vbnv, errmsg;
    auto dev = pcidev::get_dev(index);
    if (!dev->is_ready) {
        std::cerr << "device [" << index << "] is not ready, reset command exiting" << std::endl;
        return -EINVAL;
    }
    dev->sysfs_get( "rom", "VBNV", errmsg, vbnv );
    if (!errmsg.empty()) {
        std::cerr << errmsg << std::endl;
        return -EINVAL;
    }
    if (vbnv.find("_u30_") != std::string::npos) {
        /*
         * u30 reset relies on working SC and SN info. SN is read and saved
         * when FPGA is ready. so even if there is firewall trip now, we expect
         * to be able to get S/N again
         * Having SN info available also implies there is a working SC
         */
        std::string sn;
        dev->sysfs_get( "xmc", "serial_num", errmsg, sn );
        if (!errmsg.empty()) {
            std::cerr << errmsg << std::endl;
            return -EINVAL;
        }
        if (sn.empty()) {
            std::cerr << "Reset relies on S/N, but S/N can't be read from SC" << std::endl;
            return -EINVAL;
        }
        std::cout << "Card level reset. This will reset all FPGAs on the card." << std::endl;
    }

    std::cout << "All existing processes will be killed." << std::endl;
    if (!canProceed())
        return -ECANCELED;

    std::unique_ptr<device> d = xclGetDevice(index);
    if (!d)
        return -EINVAL;

    int err = d->reset(XCL_USER_RESET);
    if (err)
        std::cout << "ERROR: " << strerror(std::abs(err)) << std::endl;
    return err;
}

static int p2ptest_set_or_cmp(char *boptr, size_t size, char pattern, bool set)
{
    int stride = getpagesize();

    assert((size % stride) == 0);
    for (size_t i = 0; i < size; i += stride) {
        if (set) {
            boptr[i] = pattern;
        } else if (boptr[i] != pattern) {
            std::cout << "Error doing P2P comparison, expecting '" << pattern
                << "', saw '" << boptr[i] << std::endl;
            return -EINVAL;
        }
    }

    return 0;
}

static int p2ptest_chunk(xclDeviceHandle handle, char *boptr,
    uint64_t dev_addr, uint64_t size)
{
    char *buf = nullptr;
    char patternA = 'A';
    char patternB = 'B';

    if (xrt_core::posix_memalign((void **)&buf, getpagesize(), size))
          return -ENOMEM;

    (void) p2ptest_set_or_cmp(buf, size, patternA, true);

    if (xclUnmgdPwrite(handle, 0, buf, size, dev_addr) < 0) {
        std::cout << "Error (" << strerror (errno) << ") writing 0x"
            << std::hex << size << " bytes to 0x" << std::hex << dev_addr
            << std::dec << std::endl;
        free(buf);
        return -EIO;
    }

    if (p2ptest_set_or_cmp(boptr, size, patternA, false) != 0) {
        free(buf);
        return -EINVAL;
    }

    (void) p2ptest_set_or_cmp(boptr, size, patternB, true);

    if (xclUnmgdPread(handle, 0, buf, size, dev_addr) < 0) {
        std::cout << "Error (" << strerror (errno) << ") reading 0x"
            << std::hex << size << " bytes from 0x" << std::hex << dev_addr
            << std::dec << std::endl;
        free(buf);
        return -EIO;
    }

    if (p2ptest_set_or_cmp(buf, size, patternB, false) != 0) {
        free(buf);
        return -EINVAL;
    }

    free(buf);
    return 0;
}

static int p2ptest_bank(xclDeviceHandle handle, int memidx,
    uint64_t addr, uint64_t size)
{
    const size_t chunk_size = 16 * 1024 * 1024;
    int ret = 0;

    unsigned int boh = xclAllocBO(handle, size, 0,
        XCL_BO_FLAGS_P2P | memidx);
    if (boh == NULLBO) {
        std::cout << "Error allocating P2P BO" << std::endl;
        return -ENOMEM;
    }

    char *boptr = (char *)xclMapBO(handle, boh, true);
    if (boptr == nullptr) {
        std::cout << "Error mapping P2P BO" << std::endl;
        xclFreeBO(handle, boh);
        return -EINVAL;
    }

    int ci = 0;
    for (size_t c = 0; c < size; c += chunk_size, ci++) {
        if (p2ptest_chunk(handle, boptr + c, addr + c, chunk_size) != 0) {
            std::cout << "Error P2P testing at offset 0x" << std::hex << c
                << " on memory index " << std::dec << memidx << std::endl;
            ret = -EINVAL;
            break;
        }
        if (ci % (size / chunk_size / 16) == 0)
            std::cout << "." << std::flush;
    }

    (void) munmap(boptr, size);
    xclFreeBO(handle, boh);
    return ret;
}

/*
 * p2ptest
 */
int xcldev::device::testP2p()
{
    std::string errmsg, vbnv;
    std::vector<char> buf;
    int ret = 0;
    int p2p_enabled;
    xclbin_lock xclbin_lock(m_handle, m_idx);
    auto dev = pcidev::get_dev(m_idx);

    if (dev == nullptr)
        return -EINVAL;

    dev->sysfs_get("rom", "VBNV", errmsg, vbnv);
    p2p_enabled = pcidev::check_p2p_config(dev, errmsg);
    if (p2p_enabled != 1) {
        std::cout << "P2P BAR is not enabled. Skipping validation" << std::endl;
        return -EOPNOTSUPP;
    }

    dev->sysfs_get("icap", "mem_topology", errmsg, buf);

    const mem_topology *map = (mem_topology *)buf.data();
    if (buf.empty() || map->m_count == 0) {
        std::cout << "WARNING: 'mem_topology' invalid, "
            << "unable to perform P2P Test. Has the bitstream been loaded? "
            << "See 'xbutil program'." << std::endl;
        return -EINVAL;
    }

    for (int32_t i = 0; i < map->m_count && ret == 0; i++) {
        std::vector<std::string> supList = { "HBM", "bank" };

        //p2p is not supported for DDR on u280
        if(vbnv.find("_u280_") == std::string::npos)
            supList.push_back("DDR");

        const std::string name(reinterpret_cast<const char *>(map->m_mem_data[i].m_tag));
        bool find = false;
        for (auto s : supList) {
            if (name.compare(0, s.size(), s) == 0) {
                find = true;
                break;
            }
        }
        if (!find || !map->m_mem_data[i].m_used)
            continue;

        std::cout << "Performing P2P Test on " << map->m_mem_data[i].m_tag << " ";
        ret = p2ptest_bank(m_handle, i, map->m_mem_data[i].m_base_address,
            map->m_mem_data[i].m_size << 10);
        std::cout << std::endl;
    }

    return ret;
}

int xcldev::device::setP2p(bool enable, bool force)
{
    return xclP2pEnable(m_handle, enable, force);
}

int xcldev::xclP2p(int argc, char *argv[])
{
    int c;
    unsigned index = 0;
    int p2p_enable = -1;
    bool validate = false;
    const std::string usage("Options: [-d index] --[enable|disable|validate]");
    static struct option long_options[] = {
        {"enable", no_argument, 0, xcldev::P2P_ENABLE},
        {"disable", no_argument, 0, xcldev::P2P_DISABLE},
        {"validate", no_argument, 0, xcldev::P2P_VALIDATE},
        {0, 0, 0, 0}
    };
    int long_index, ret;
    const char* short_options = "d:f"; //don't add numbers
    const char* exe = argv[ 0 ];
    bool force = false;

    while ((c = getopt_long(argc, argv, short_options, long_options,
        &long_index)) != -1) {
        switch (c) {
        case 'd':
            ret = str2index(optarg, index);
            if (ret != 0)
                return ret;
            break;
        case 'f':
            force = true;
            break;
        case xcldev::P2P_ENABLE:
            p2p_enable = 1;
            break;
        case xcldev::P2P_DISABLE:
            p2p_enable = 0;
            break;
        case xcldev::P2P_VALIDATE:
            validate = true;
            break;
        default:
            xcldev::printHelp(exe);
            return 1;
        }
    }

    std::unique_ptr<device> d = xclGetDevice(index);
    if (!d)
        return -EINVAL;

    if (validate)
        return d->testP2p();

    if (p2p_enable == -1) {
        std::cerr << usage << std::endl;
        return -EINVAL;
    }

    ret = isSudo();
    if (ret)
        return ret;

    ret = d->setP2p(p2p_enable, force);
    if (ret)
        std::cout << "Config P2P failed" << std::endl;
    else if (p2p_enable)
        std::cout << "Please WARM reboot to enable p2p now." << std::endl;
    else
        std::cout << "Please WARM reboot to disable p2p now." << std::endl;

    return 0;
}

int xcldev::device::setCma(bool enable, uint64_t total_size)
{
    return xclCmaEnable(m_handle, enable, total_size);
}

int xcldev::xclCma(int argc, char *argv[])
{
    int c;
    unsigned int index = 0;
    int cma_enable = -1;
    uint64_t total_size = 0, unit_sz = 0;
    const std::string usage("Options: [-d index] --[enable|disable] --size [size M|G]");
    static struct option long_options[] = {
        {"enable", no_argument, 0, xcldev::CMA_ENABLE},
        {"disable", no_argument, 0, xcldev::CMA_DISABLE},
        {"size", required_argument, nullptr, xcldev::CMA_SIZE},
        {0, 0, 0, 0}
    };

    int long_index, ret;
    const char* short_options = "d:"; //don't add numbers
    const char* exe = argv[ 0 ];
    std::string optarg_s, unit_str;
    const char *unit = NULL;
    size_t end = 0;

    while ((c = getopt_long(argc, argv, short_options, long_options,
        &long_index)) != -1) {
        switch (c) {
        case 'd':
            ret = str2index(optarg, index);
            if (ret != 0)
                return ret;
            break;
        case xcldev::CMA_ENABLE:
            cma_enable = 1;
            break;
        case xcldev::CMA_DISABLE:
            cma_enable = 0;
            break;
        case xcldev::CMA_SIZE:
            optarg_s += optarg;
            try {
                total_size = std::stoll(optarg_s, &end, 0);
            } catch (const std::exception& ex) {
                //out of range, invalid argument ex
                std::cout << "ERROR: Value supplied to --size option is invalid\n";
                return -1;
            }
            unit_str = optarg_s.substr(end);
            unit = unit_str.c_str();

            if (std::tolower(unit[0]) == 'm')
                unit_sz = 1024*1024;
            else if (std::tolower(unit[0]) == 'g')
                unit_sz = 1024*1024*1024;

            total_size *= unit_sz;
            break;
        default:
            xcldev::printHelp(exe);
            return 1;
        }
    }

    std::unique_ptr<device> d = xclGetDevice(index);
    if (!d)
        return -EINVAL;

    if (cma_enable == -1) {
        std::cerr << usage << std::endl;
        return -EINVAL;
    }

    if (cma_enable && !total_size) {
        std::cerr << usage << std::endl;
        return -EINVAL;
    }

    ret = isSudo();
    if (ret)
        return ret;

    /* At this moment, we have two way to collect CMA memory chunk
     * 1. Call Kernel API
     * 2. Huge Page MMAP
     */
    ret = d->setCma(cma_enable, total_size);
    if (ret == -ENOMEM) {
        std::cout << "ERROR: No enough HOST MEM." << std::endl;
        std::cout << "Please check grub settings" << std::endl;
    } else if (ret == -EINVAL) {
        std::cout << "ERROR: Invalid HOST MEM size." << std::endl;
    } else if (ret == -ENXIO) {
        std::cout << "ERROR: Huge page is not supported on this platform"
            << std::endl;
    } else if (ret == -ENODEV) {
        std::cout << "ERROR: Does not support HOST MEM feature"
            << std::endl;
    } else if (ret == -EBUSY) {
        std::cout << "ERROR: HOST MEM already enabled or in-use"
            << std::endl;
    } else if (!ret) {
        std::cout << "xbutil host_mem done successfully" << std::endl;
    } else if (ret) {
        std::cout << "ERROR: " << strerror(std::abs(ret)) << std::endl;
    }

    return ret;
}

/*
 * m2mtest
 */
static void m2m_free_unmap_bo(xclDeviceHandle handle, unsigned boh,
    void * boptr, size_t boSize)
{
    if(boptr != nullptr)
        munmap(boptr, boSize);
    if(boh != NULLBO)
        xclFreeBO(handle, boh);
}

static int m2m_alloc_init_bo(xclDeviceHandle handle, unsigned &boh,
    char * &boptr, size_t boSize, int bank, char pattern)
{
    boh = xclAllocBO(handle, boSize, 0, bank);
    if (boh == NULLBO) {
        std::cout << "Error allocating BO" << std::endl;
        return -ENOMEM;
    }
    boptr = (char*) xclMapBO(handle, boh, true);
    if (boptr == nullptr) {
        std::cout << "Error mapping BO" << std::endl;
        m2m_free_unmap_bo(handle, boh, boptr, boSize);
        return -EINVAL;
    }
    memset(boptr, pattern, boSize);
    int err = xclSyncBO(handle, boh, XCL_BO_SYNC_BO_TO_DEVICE, boSize, 0);
    if (err) {
        std::cout << "ERROR: Unable to sync BO, err: " << err << std::endl;
        m2m_free_unmap_bo(handle, boh, boptr, boSize);
        return -EINVAL;
    }
    return 0;
}

static int m2mtest_bank(xclDeviceHandle handle, int bank_a, int bank_b)
{
    unsigned boSrc = NULLBO;
    unsigned boTgt = NULLBO;
    char *boSrcPtr = nullptr;
    char *boTgtPtr = nullptr;
    int ret = 0;

    //Allocate and init boSrc
    if(m2m_alloc_init_bo(handle, boSrc, boSrcPtr, m2mBoSize, bank_a, 'A'))
        return -EINVAL;

    //Allocate and init boTgt
    if(m2m_alloc_init_bo(handle, boTgt, boTgtPtr, m2mBoSize, bank_b, 'B')) {
        m2m_free_unmap_bo(handle, boSrc, boSrcPtr, m2mBoSize);
        return -EINVAL;
    }

    xcldev::Timer timer;
    if ((ret = xclCopyBO(handle, boTgt, boSrc, m2mBoSize, 0, 0)))
        return ret;
    double timer_stop = timer.stop();

    if(xclSyncBO(handle, boTgt, XCL_BO_SYNC_BO_FROM_DEVICE, m2mBoSize, 0)) {
        m2m_free_unmap_bo(handle, boSrc, boSrcPtr, m2mBoSize);
        m2m_free_unmap_bo(handle, boTgt, boTgtPtr, m2mBoSize);
        std::cout << "ERROR: Unable to sync target BO" << std::endl;
        return -EINVAL;
    }

    bool match = (memcmp(boSrcPtr, boTgtPtr, m2mBoSize) == 0);

    // Clean up
    m2m_free_unmap_bo(handle, boSrc, boSrcPtr, m2mBoSize);
    m2m_free_unmap_bo(handle, boTgt, boTgtPtr, m2mBoSize);

    if (!match) {
        std::cout << "Memory comparison failed" << std::endl;
        return -EINVAL;
    }

    //bandwidth
    double total = m2mBoSize;
    total *= 1000000; // convert us to s
    total /= (1024 * 1024); //convert to MB
    std::cout << total / timer_stop << " MB/s\t\n";

    return 0;
}

int xcldev::device::testM2m()
{
    std::string errmsg, vbnv;
    std::vector<char> buf;
    int m2m_enabled = 0;
    std::vector<mem_data> usedBanks;
    int ret = 0;
    xclbin_lock xclbin_lock(m_handle, m_idx);
    auto dev = pcidev::get_dev(m_idx);
    uint32_t kds_mode;

    if (dev == nullptr)
        return -EINVAL;

    dev->sysfs_get<uint32_t>("", "kds_mode", errmsg, kds_mode, 0);

    if (!kds_mode)
        dev->sysfs_get<int>("mb_scheduler", "kds_numcdmas", errmsg, m2m_enabled, 0);
    else
        dev->sysfs_get<int>("", "kds_numcdma", errmsg, m2m_enabled, 0);

    // Workaround:
    // u250_xdma_201830_1 falsely shows that m2m is available
    // which causes a hang. Skip m2mtest if this platform is installed
    dev->sysfs_get( "rom", "VBNV", errmsg, vbnv );
    if (m2m_enabled == 0 || strstr( vbnv.c_str(), "_u250_xdma_201830_1")) {
        std::cout << "M2M is not available. Skipping validation" << std::endl;
        return -EOPNOTSUPP;
    }

    dev->sysfs_get("icap", "mem_topology", errmsg, buf);
    mem_topology *map = (mem_topology *)buf.data();

    if(buf.empty() || map->m_count == 0) {
        std::cout << "WARNING: 'mem_topology' invalid, "
            << "unable to perform M2M Test. Has the bitstream been loaded? "
            << "See 'xbutil program'." << std::endl;
        return -EINVAL;
    }

    for(int32_t i = 0; i < map->m_count; i++) {
        if (isHostMem(map->m_mem_data[i].m_tag))
            continue;

        if(map->m_mem_data[i].m_used &&
            map->m_mem_data[i].m_size * 1024 >= m2mBoSize) {
            /* use u8 m_type field to as bank index */
            map->m_mem_data[i].m_type = i;
            usedBanks.insert(usedBanks.end(), map->m_mem_data[i]);
        }
    }

    if (usedBanks.size() <= 1) {
        std::cout << "Only one bank available. Skipping validation" << std::endl;
        return ret;
    }

    for(uint i = 0; i < usedBanks.size()-1; i++) {
        for(uint j = i+1; j < usedBanks.size(); j++) {
            std::cout << usedBanks[i].m_tag << " -> "
                << usedBanks[j].m_tag << " M2M bandwidth: ";
            if (!usedBanks[i].m_size || !usedBanks[j].m_size)
                continue;

            ret = m2mtest_bank(m_handle, usedBanks[i].m_type, usedBanks[j].m_type);
            if(ret != 0)
                return ret;
        }
    }
    return ret;
}

/*
 * iops test
 */
int
xcldev::device::iopsTest()
{
    std::string output;

    //TODO: use xrt_iops_test.exe after XRT API supports construct device by BDF
    int ret = runTestCase(std::string("xcl_iops_test.exe"), std::string("verify.xclbin"),
                output, std::string(" -t 1 -l 128 -a 500000"));

    if (ret != 0) {
        if (!output.empty())
            std::cout << output << std::endl;
        return ret;
    }

    if (output.find("Overall Commands") == std::string::npos) {
        std::cout << output << std::endl;
        ret = -EINVAL;
    }

    // Find start and end point of "IOPS: xxxxxx" substring
    auto sp = output.find("IOPS:");
    auto ep = output.find("\n", sp);
    if (sp == std::string::npos || ep == std::string::npos) {
        std::cout << "IOPS print result unexpected" << std::endl;
        ret = -EINVAL;
    }
    else {
        std::cout << "\rMaximum " << output.substr(sp, ep - sp) << std::endl;
    }
    return ret;
}

int
xcldev::device::iopsTestWithArgs(const std::string& name, const std::string& args)
{
    std::string output;

    int ret = runTestCase(name, std::string("verify.xclbin"), output, args);
    if (ret != 0)
        return ret;

    std::cout << output << std::endl;
    return 0;
}

int xcldev::xclScheduler(int argc, char *argv[])
{
    static struct option long_opts[] = {
        {"echo", required_argument, 0, 'e'},
        {"kds_schedule", required_argument, 0, 'k'},
        {"cu_intr", required_argument, 0, xcldev::KDS_CU_INTERRUPT},
        {"test", required_argument, 0, xcldev::KDS_TEST},
        {"args", required_argument, 0, xcldev::KDS_ARGS},
        {"show", no_argument, 0, xcldev::KDS_SHOW},
        {0, 0, 0, 0}
    };
    const char* short_opts = "d:e:k:";
    int c, opt_idx;
    std::string errmsg;
    unsigned index = 0;
    int kds_echo = -1;
    int ert_disable = -1;
    int cu_intr = -1;
    int kds_show = -1;
    std::string test;
    std::string args;

    int ret = isSudo();
    if (ret)
        return ret;

    while ((c = getopt_long(argc, argv, short_opts, long_opts, &opt_idx)) != -1) {
        switch (c) {
        case 'd': {
            int ret = str2index(optarg, index);
            if (ret != 0)
                return ret;
            if (index >= pcidev::get_dev_total()) {
                std::cout << "ERROR: index " << index << " out of range"
                    << std::endl;
                return -EINVAL;
            }
            break;
        }
        case 'e':
            kds_echo = std::atoi(optarg);
            break;
        case 'k':
            ert_disable = std::atoi(optarg);
            break;
        case xcldev::KDS_CU_INTERRUPT:
            cu_intr = std::atoi(optarg);
            break;
        case xcldev::KDS_TEST:
            test = optarg;
            break;
        case xcldev::KDS_ARGS:
            args = optarg;
            break;
        case xcldev::KDS_SHOW:
            kds_show = 1;
            break;
        default:
            /* This is hidden command, silently exit */
            return -EINVAL;
        }
    }

    if (kds_show) {
            std::string value;

            pcidev::get_dev(index)->sysfs_get( "", "ert_disable", errmsg, value);
            std::cout << "Device[" << index << "] ert_disable: " << value << std::endl;

            pcidev::get_dev(index)->sysfs_get( "", "kds_interrupt", errmsg, value);
            std::cout << "Device[" << index << "] interrupt mode: " << value << std::endl;

            return 0;
    }

    if (kds_echo != -1) {
        std::string val = (kds_echo == 0)? "0" : "1";
        pcidev::get_dev(index)->sysfs_put( "", "kds_echo", errmsg, val);
        if (!errmsg.empty()) {
            std::cout << errmsg << std::endl;
            return -EINVAL;
        }
        std::string kds_echo;
        pcidev::get_dev(index)->sysfs_get( "", "kds_echo", errmsg, kds_echo);
        std::cout << "Device[" << index << "] kds_echo: " << kds_echo << std::endl;
    }

    if (ert_disable != -1) {
        std::string val = (ert_disable == 0)? "0" : "1";
        pcidev::get_dev(index)->sysfs_put( "", "ert_disable", errmsg, val);
        if (!errmsg.empty()) {
            std::cout << errmsg << std::endl;
            return -EINVAL;
        }
        std::string ert_disable;
        pcidev::get_dev(index)->sysfs_get( "", "ert_disable", errmsg, ert_disable);
        std::cout << "Device[" << index << "] ert_disable: " << ert_disable << std::endl;
    }

    if (cu_intr != -1) {
        std::string val = (cu_intr == 1)? "cu" : "ert";
        pcidev::get_dev(index)->sysfs_put( "", "kds_interrupt", errmsg, val);
        if (!errmsg.empty()) {
            std::cout << errmsg << std::endl;
            return -EINVAL;
        }
        std::string kds_interrupt;
        pcidev::get_dev(index)->sysfs_get( "", "kds_interrupt", errmsg, kds_interrupt);
        std::cout << "Device[" << index << "] interrupt mode: " << kds_interrupt << std::endl;
    }

    std::unique_ptr<device> dev = xclGetDevice(index);
    if (!dev) {
        std::cout << "ERROR: Can't open card[" << index << "]" << std::endl;
        return -ENOENT;
    }

    if (!test.empty()) {
        int ret = 0;
        if (test.compare("xcl_iops") == 0) {
            test = "xcl_iops_test.exe";
        } else if (test.compare("xrt_iops") == 0) {
            test = "xrt_iops_test.exe";
        } else {
            std::cout << "ERROR:Unknown test" << std::endl;
            return -EINVAL;
        }

        args = " " + args;
        ret = dev->iopsTestWithArgs(test, args);
        if (ret == -EOPNOTSUPP)
            std::cout << "Test not support" << std::endl;
        else if (ret < 0)
            std::cout << "Test failed" << std::endl;
        else
            std::cout << "Test completed" << std::endl;
    }

    return 0;
}
