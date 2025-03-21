// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

/*++

Module Name:

    cgroup.cpp

Abstract:
    Read the memory limit for the current process
--*/
#ifdef __FreeBSD__
#define _WITH_GETLINE
#endif

#include <cstdint>
#include <cassert>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/mount.h>
#elif !defined(__HAIKU__)
#include <sys/vfs.h>
#endif
#include <errno.h>
#include <limits>

#include "config.gc.h"

#include "cgroup.h"

#ifndef SIZE_T_MAX
#define SIZE_T_MAX (~(size_t)0)
#endif

#define CGROUP2_SUPER_MAGIC 0x63677270

#define PROC_MOUNTINFO_FILENAME "/proc/self/mountinfo"
#define PROC_CGROUP_FILENAME "/proc/self/cgroup"
#define PROC_STATM_FILENAME "/proc/self/statm"
#define CGROUP1_MEMORY_LIMIT_FILENAME "/memory.limit_in_bytes"
#define CGROUP2_MEMORY_LIMIT_FILENAME "/memory.max"
#define CGROUP_MEMORY_STAT_FILENAME "/memory.stat"
#define CGROUP1_MEMORY_USAGE_FILENAME "/memory.usage_in_bytes"
#define CGROUP2_MEMORY_USAGE_FILENAME "/memory.current"
#define CGROUP1_MEMORY_USE_HIERARCHY_FILENAME "/memory.use_hierarchy"
#define CGROUP1_MEMORY_STAT_HIERARCHICAL_MEMORY_LIMIT_FIELD "hierarchical_memory_limit "
#define CGROUP1_MEMORY_STAT_INACTIVE_FIELD "total_inactive_file "
#define CGROUP2_MEMORY_STAT_INACTIVE_FIELD "inactive_file "

extern bool ReadMemoryValueFromFile(const char* filename, uint64_t* val);

namespace
{
class CGroup
{
    // the cgroup version number or 0 to indicate cgroups are not found or not enabled
    static int s_cgroup_version;

    static char *s_memory_cgroup_path;
    static char *s_memory_cgroup_hierarchy_mount;
public:
    static void Initialize()
    {
        s_cgroup_version = FindCGroupVersion();
        FindCGroupPath(s_cgroup_version == 1 ? &IsCGroup1MemorySubsystem : nullptr, &s_memory_cgroup_path, &s_memory_cgroup_hierarchy_mount);
    }

    static void Cleanup()
    {
        free(s_memory_cgroup_path);
        free(s_memory_cgroup_hierarchy_mount);
    }

    static bool GetPhysicalMemoryLimit(uint64_t *val)
    {
        if (s_cgroup_version == 0)
            return false;
        else if (s_cgroup_version == 1)
            return GetCGroupMemoryLimitV1(val);
        else if (s_cgroup_version == 2)
            return GetCGroupMemoryLimitV2(val);
        else
        {
            assert(!"Unknown cgroup version.");
            return false;
        }
    }

    static bool GetPhysicalMemoryUsage(size_t *val)
    {
        if (s_cgroup_version == 0)
            return false;
        else if (s_cgroup_version == 1)
            return GetCGroupMemoryUsage(val, CGROUP1_MEMORY_USAGE_FILENAME, CGROUP1_MEMORY_STAT_INACTIVE_FIELD);
        else if (s_cgroup_version == 2)
            return GetCGroupMemoryUsage(val, CGROUP2_MEMORY_USAGE_FILENAME, CGROUP2_MEMORY_STAT_INACTIVE_FIELD);
        else
        {
            assert(!"Unknown cgroup version.");
            return false;
        }
    }

private:
    static int FindCGroupVersion()
    {
        // It is possible to have both cgroup v1 and v2 enabled on a system.
        // Most non-bleeding-edge Linux distributions fall in this group. We
        // look at the file system type of /sys/fs/cgroup to determine which
        // one is the default. For more details, see:
        // https://systemd.io/CGROUP_DELEGATION/#three-different-tree-setups-
        // We dont care about the difference between the "legacy" and "hybrid"
        // modes because both of those involve cgroup v1 controllers managing
        // resources.

#if !HAVE_NON_LEGACY_STATFS
        return 0;
#else

        struct statfs stats;
        int result = statfs("/sys/fs/cgroup", &stats);
        if (result != 0)
            return 0;

        if (stats.f_type == CGROUP2_SUPER_MAGIC)
        {
            return 2;
        }
        else
        {
            // Assume that if /sys/fs/cgroup exists and the file system type is not cgroup2fs,
            // it is cgroup v1. Typically the file system type is tmpfs, but other values have
            // been seen in the wild.
            return 1;
        }
#endif
    }

    static bool IsCGroup1MemorySubsystem(const char *strTok){
        return strcmp("memory", strTok) == 0;
    }

        static void FindCGroupPath(bool (*is_subsystem)(const char *), char** pcgroup_path, char ** pcgroup_hierarchy_mount = nullptr){

        char *cgroup_path = nullptr;
        char *hierarchy_mount = nullptr;
        char *hierarchy_root = nullptr;
        char *cgroup_path_relative_to_mount = nullptr;
        size_t common_path_prefix_len;

        FindHierarchyMount(is_subsystem, &hierarchy_mount, &hierarchy_root);
        if (hierarchy_mount == nullptr || hierarchy_root == nullptr)
            goto done;

        cgroup_path_relative_to_mount = FindCGroupPathForSubsystem(is_subsystem);
        if (cgroup_path_relative_to_mount == nullptr)
            goto done;

        cgroup_path = (char*)malloc(strlen(hierarchy_mount) + strlen(cgroup_path_relative_to_mount) + 1);
        if (cgroup_path == nullptr)
           goto done;

        strcpy(cgroup_path, hierarchy_mount);
        // For a host cgroup, we need to append the relative path.
        // The root and cgroup path can share a common prefix of the path that should not be appended.
        // Example 1 (docker):
        // hierarchy_mount:               /sys/fs/cgroup/cpu
        // hierarchy_root:                /docker/87ee2de57e51bc75175a4d2e81b71d162811b179d549d6601ed70b58cad83578
        // cgroup_path_relative_to_mount: /docker/87ee2de57e51bc75175a4d2e81b71d162811b179d549d6601ed70b58cad83578/my_named_cgroup
        // append do the cgroup_path:     /my_named_cgroup
        // final cgroup_path:             /sys/fs/cgroup/cpu/my_named_cgroup
        //
        // Example 2 (out of docker)
        // hierarchy_mount:               /sys/fs/cgroup/cpu
        // hierarchy_root:                /
        // cgroup_path_relative_to_mount: /my_named_cgroup
        // append do the cgroup_path:     /my_named_cgroup
        // final cgroup_path:             /sys/fs/cgroup/cpu/my_named_cgroup
        common_path_prefix_len = strlen(hierarchy_root);
        if ((common_path_prefix_len == 1) || strncmp(hierarchy_root, cgroup_path_relative_to_mount, common_path_prefix_len) != 0)
        {
            common_path_prefix_len = 0;
        }

        assert((cgroup_path_relative_to_mount[common_path_prefix_len] == '/') || (cgroup_path_relative_to_mount[common_path_prefix_len] == '\0'));

        strcat(cgroup_path, cgroup_path_relative_to_mount + common_path_prefix_len);


    done:
        free(hierarchy_root);
        free(cgroup_path_relative_to_mount);
        *pcgroup_path = cgroup_path;
        if (pcgroup_hierarchy_mount != nullptr)
        {
            *pcgroup_hierarchy_mount = hierarchy_mount;
        }
        else
        {
            free(hierarchy_mount);
        }
    }

    static void FindHierarchyMount(bool (*is_subsystem)(const char *), char** pmountpath, char** pmountroot)
    {
        char *line = nullptr;
        size_t lineLen = 0, maxLineLen = 0;
        char *filesystemType = nullptr;
        char *options = nullptr;
        char *mountpath = nullptr;
        char *mountroot = nullptr;

        FILE *mountinfofile = fopen(PROC_MOUNTINFO_FILENAME, "r");
        if (mountinfofile == nullptr)
            goto done;

        while (getline(&line, &lineLen, mountinfofile) != -1)
        {
            if (filesystemType == nullptr || lineLen > maxLineLen)
            {
                free(filesystemType);
                filesystemType = nullptr;
                free(options);
                options = nullptr;
                filesystemType = (char*)malloc(lineLen+1);
                if (filesystemType == nullptr)
                    goto done;
                options = (char*)malloc(lineLen+1);
                if (options == nullptr)
                    goto done;
                maxLineLen = lineLen;
            }

            char* separatorChar = strstr(line, " - ");

            // See man page of proc to get format for /proc/self/mountinfo file
            int sscanfRet = sscanf(separatorChar,
                                   " - %s %*s %s",
                                   filesystemType,
                                   options);
            if (sscanfRet != 2)
            {
                assert(!"Failed to parse mount info file contents with sscanf.");
                goto done;
            }

            if (strncmp(filesystemType, "cgroup", 6) == 0)
            {
                bool isSubsystemMatch = is_subsystem == nullptr;
                if (!isSubsystemMatch)
                {
                    char* context = nullptr;
                    char* strTok = strtok_r(options, ",", &context);
                    while (!isSubsystemMatch && strTok != nullptr)
                    {
                        isSubsystemMatch = is_subsystem(strTok);
                        strTok = strtok_r(nullptr, ",", &context);
                    }
                }
                if (isSubsystemMatch)
                {
                        mountpath = (char*)malloc(lineLen+1);
                        if (mountpath == nullptr)
                            goto done;
                        mountroot = (char*)malloc(lineLen+1);
                        if (mountroot == nullptr)
                            goto done;

                        sscanfRet = sscanf(line,
                                           "%*s %*s %*s %s %s ",
                                           mountroot,
                                           mountpath);
                        if (sscanfRet != 2)
                            assert(!"Failed to parse mount info file contents with sscanf.");

                        // assign the output arguments and clear the locals so we don't free them.
                        *pmountpath = mountpath;
                        *pmountroot = mountroot;
                        mountpath = mountroot = nullptr;
                }
            }
        }
    done:
        free(mountpath);
        free(mountroot);
        free(filesystemType);
        free(options);
        free(line);
        if (mountinfofile)
            fclose(mountinfofile);
    }

    static char* FindCGroupPathForSubsystem(bool (*is_subsystem)(const char *))
    {
        char *line = nullptr;
        size_t lineLen = 0;
        size_t maxLineLen = 0;
        char *subsystem_list = nullptr;
        char *cgroup_path = nullptr;
        bool result = false;

        FILE *cgroupfile = fopen(PROC_CGROUP_FILENAME, "r");
        if (cgroupfile == nullptr)
            goto done;

        while (!result && getline(&line, &lineLen, cgroupfile) != -1)
        {
            if (subsystem_list == nullptr || lineLen > maxLineLen)
            {
                free(subsystem_list);
                subsystem_list = nullptr;
                free(cgroup_path);
                cgroup_path = nullptr;
                subsystem_list = (char*)malloc(lineLen+1);
                if (subsystem_list == nullptr)
                    goto done;
                cgroup_path = (char*)malloc(lineLen+1);
                if (cgroup_path == nullptr)
                    goto done;
                maxLineLen = lineLen;
            }

            if (s_cgroup_version == 1)
            {
                // See man page of proc to get format for /proc/self/cgroup file
                int sscanfRet = sscanf(line,
                                       "%*[^:]:%[^:]:%s",
                                       subsystem_list,
                                       cgroup_path);
                if (sscanfRet != 2)
                {
                    assert(!"Failed to parse cgroup info file contents with sscanf.");
                    goto done;
                }

                char* context = nullptr;
                char* strTok = strtok_r(subsystem_list, ",", &context);
                while (strTok != nullptr)
                {
                    if (is_subsystem(strTok))
                    {
                        result = true;
                        break;
                    }
                    strTok = strtok_r(nullptr, ",", &context);
                }
            }
            else if (s_cgroup_version == 2)
            {
                // See https://www.kernel.org/doc/Documentation/cgroup-v2.txt
                // Look for a "0::/some/path"
                int sscanfRet = sscanf(line,
                                       "0::%s",
                                       cgroup_path);
                if (sscanfRet == 1)
                {
                    result = true;
                }
            }
            else
            {
                assert(!"Unknown cgroup version in mountinfo.");
                goto done;
            }
        }
    done:
        free(subsystem_list);
        if (!result)
        {
            free(cgroup_path);
            cgroup_path = nullptr;
        }
        free(line);
        if (cgroupfile)
            fclose(cgroupfile);
        return cgroup_path;
    }

    static bool GetCGroupMemoryStatField(const char *fieldName, uint64_t *val)
    {
        if (s_memory_cgroup_path == nullptr)
            return false;

        char* stat_filename = nullptr;
        if (asprintf(&stat_filename, "%s%s", s_memory_cgroup_path, CGROUP_MEMORY_STAT_FILENAME) < 0)
            return false;

        FILE *stat_file = fopen(stat_filename, "r");
        free(stat_filename);
        if (stat_file == nullptr)
            return false;

        char *line = nullptr;
        size_t lineLen = 0;
        bool foundFieldValue = false;
        char* endptr;

        size_t fieldNameLength = strlen(fieldName);

        while (getline(&line, &lineLen, stat_file) != -1)
        {
            if (strncmp(line, fieldName, fieldNameLength) == 0)
            {
                errno = 0;
                const char* startptr = line + fieldNameLength;
                size_t fieldValue = strtoll(startptr, &endptr, 10);
                if (endptr != startptr && errno == 0)
                {
                    foundFieldValue = true;
                    *val = fieldValue;
                }

                break;
            }
        }

        fclose(stat_file);
        free(line);

        return foundFieldValue;
    }

    static bool GetCGroupMemoryLimitV1(uint64_t *val)
    {
        if (s_memory_cgroup_path == nullptr)
            return false;

        char* mem_use_hierarchy_filename = nullptr;
        if (asprintf(&mem_use_hierarchy_filename, "%s%s", s_memory_cgroup_path, CGROUP1_MEMORY_USE_HIERARCHY_FILENAME) < 0)
            return false;

        uint64_t useHierarchy = 0;
        ReadMemoryValueFromFile(mem_use_hierarchy_filename, &useHierarchy);
        free(mem_use_hierarchy_filename);

        if (useHierarchy)
        {
            return GetCGroupMemoryStatField(CGROUP1_MEMORY_STAT_HIERARCHICAL_MEMORY_LIMIT_FIELD, val);
        }

        char* mem_limit_filename = nullptr;
        if (asprintf(&mem_limit_filename, "%s%s", s_memory_cgroup_path, CGROUP1_MEMORY_LIMIT_FILENAME) < 0)
            return false;

        bool result = ReadMemoryValueFromFile(mem_limit_filename, val);
        free(mem_limit_filename);
        return result;
    }
    
    static bool GetCGroupMemoryLimitV2(uint64_t *val)
    {
        if (s_memory_cgroup_path == nullptr)
            return false;

        // Process the whole CGroup hierarchy to find a level with the most limiting limit
        size_t memory_cgroup_hierarchy_mount_length = strlen(s_memory_cgroup_hierarchy_mount);
        uint64_t min_limit = std::numeric_limits<uint64_t>::max();
        uint64_t limit;
        bool found_any_limit = false;

        char *mem_limit_filename = nullptr;
        if (asprintf(&mem_limit_filename, "%s%s", s_memory_cgroup_path, CGROUP2_MEMORY_LIMIT_FILENAME) < 0)
            return false;

        size_t cgroupPathLength = strlen(s_memory_cgroup_path);

        // Iterate over the directory hierarchy representing the cgroup hierarchy until reaching the 
        // mount directory. The mount directory doesn't contain the memory.max.
        do
        {
            if (ReadMemoryValueFromFile(mem_limit_filename, &limit))
            {
                found_any_limit = true;
                if (limit < min_limit)
                {
                    min_limit = limit;
                }
            }

            // Get the parent cgroup memory limit file path
            char *parent_directory_end = mem_limit_filename + cgroupPathLength - 1;
            while (*parent_directory_end != '/')
            {
                parent_directory_end--;
            }

            cgroupPathLength = parent_directory_end - mem_limit_filename;

            strcpy(parent_directory_end, CGROUP2_MEMORY_LIMIT_FILENAME);
        }
        while (cgroupPathLength != memory_cgroup_hierarchy_mount_length);

        free(mem_limit_filename);

        if (found_any_limit)
        {
            *val = min_limit;
        }
        return found_any_limit;
    }

    static bool GetCGroupMemoryUsage(size_t *val, const char *filename, const char *inactiveFileFieldName)
    {
        // Use the same way to calculate memory load as popular container tools (Docker, Kubernetes, Containerd etc.)
        // For cgroup v1: value of 'memory.usage_in_bytes' minus 'total_inactive_file' value of 'memory.stat'
        // For cgroup v2: value of 'memory.current' minus 'inactive_file' value of 'memory.stat'

        char* mem_usage_filename = nullptr;
        if (asprintf(&mem_usage_filename, "%s%s", s_memory_cgroup_path, filename) < 0)
            return false;

        uint64_t usage = 0;

        bool result = ReadMemoryValueFromFile(mem_usage_filename, &usage);
        if (result)
        {
            if (usage > std::numeric_limits<size_t>::max())
            {
                usage = std::numeric_limits<size_t>::max();
            }
        }

        free(mem_usage_filename);

        if (!result)
            return result;

        if (s_memory_cgroup_path == nullptr)
            return false;

        uint64_t inactiveFileValue = 0;
        if (GetCGroupMemoryStatField(inactiveFileFieldName, &inactiveFileValue))
        {
            if (inactiveFileValue > std::numeric_limits<size_t>::max())
            {
                inactiveFileValue = std::numeric_limits<size_t>::max();
            }

            *val = (size_t)usage - (size_t)inactiveFileValue;
            return true;
        }

        return false;
    }
};
}

int CGroup::s_cgroup_version = 0;
char *CGroup::s_memory_cgroup_path = nullptr;
char *CGroup::s_memory_cgroup_hierarchy_mount = nullptr;

void InitializeCGroup()
{
    CGroup::Initialize();
}

void CleanupCGroup()
{
    CGroup::Cleanup();
}

size_t GetRestrictedPhysicalMemoryLimit()
{
    uint64_t physical_memory_limit = 0;

    if (!CGroup::GetPhysicalMemoryLimit(&physical_memory_limit))
         return 0;

    // If there's no memory limit specified on the container this
    // actually returns 0x7FFFFFFFFFFFF000 (2^63-1 rounded down to
    // 4k which is a common page size). So we know we are not
    // running in a memory restricted environment.
    if (physical_memory_limit > 0x7FFFFFFF00000000)
    {
        return 0;
    }

    struct rlimit curr_rlimit;
    size_t rlimit_soft_limit = (size_t)RLIM_INFINITY;
    if (getrlimit(RLIMIT_AS, &curr_rlimit) == 0)
    {
        rlimit_soft_limit = curr_rlimit.rlim_cur;
    }
    physical_memory_limit = (physical_memory_limit < rlimit_soft_limit) ?
                            physical_memory_limit : rlimit_soft_limit;

    // Ensure that limit is not greater than real memory size
    long pages = sysconf(_SC_PHYS_PAGES);
    if (pages != -1)
    {
        long pageSize = sysconf(_SC_PAGE_SIZE);
        if (pageSize != -1)
        {
            physical_memory_limit = (physical_memory_limit < (size_t)pages * pageSize)?
                                    physical_memory_limit : (size_t)pages * pageSize;
        }
    }

    if (physical_memory_limit > std::numeric_limits<size_t>::max())
    {
        // It is observed in practice when the memory is unrestricted, Linux control
        // group returns a physical limit that is bigger than the address space
        return std::numeric_limits<size_t>::max();
    }
    else
    {
        return (size_t)physical_memory_limit;
    }
}

bool GetPhysicalMemoryUsed(size_t* val)
{
    bool result = false;
    size_t linelen;
    char* line = nullptr;

    if (val == nullptr)
        return false;

    // Linux uses cgroup usage to trigger oom kills.
    if (CGroup::GetPhysicalMemoryUsage(val))
        return true;

    // process resident set size.
    FILE* file = fopen(PROC_STATM_FILENAME, "r");
    if (file != nullptr && getline(&line, &linelen, file) != -1)
    {
        char* context = nullptr;
        char* strTok = strtok_r(line, " ", &context);
        strTok = strtok_r(nullptr, " ", &context);

        errno = 0;
        *val = strtoull(strTok, nullptr, 0);
        if (errno == 0)
        {
            long pageSize = sysconf(_SC_PAGE_SIZE);
            if (pageSize != -1)
            {
                *val = *val * pageSize;
                result = true;
            }
        }
    }

    if (file)
        fclose(file);
    free(line);
    return result;
}
