/*
 * Copyright (c) 2015, 2016, 2017, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY LOG OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <vector>
#include <algorithm>
#include <libgen.h>
#include <iostream>
#include <fstream>
#include <streambuf>
#include <stdexcept>
#include <string>
#include <sstream>
#include <json-c/json.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>

#include "geopm.h"
#include "geopm_version.h"
#include "geopm_signal_handler.h"
#include "geopm_hash.h"
#include "Controller.hpp"
#include "Exception.hpp"
#include "config.h"

#ifndef NAME_MAX
#define NAME_MAX 1024
#endif

extern "C"
{
    static void *geopm_threaded_run(void *args)
    {
        long err = 0;
        struct geopm_ctl_c *ctl = (struct geopm_ctl_c *)args;

        err = geopm_ctl_run(ctl);

        return (void *)err;
    }


    int geopmctl_main(const char *policy_config)
    {
        int err = 0;
        try {
            if (policy_config) {
                std::string policy_config_str(policy_config);
                geopm::GlobalPolicy policy(policy_config_str, "");
                geopm::Controller ctl(&policy, MPI_COMM_WORLD);
                err = geopm_ctl_run((struct geopm_ctl_c *)&ctl);
            }
            //The null case is for all nodes except rank 0.
            //These controllers should assume their policy from the master.
            else {
                geopm::Controller ctl(NULL, MPI_COMM_WORLD);
                err = geopm_ctl_run((struct geopm_ctl_c *)&ctl);
            }
        }
        catch (...) {
            err = geopm::exception_handler(std::current_exception());
        }
        return err;
    }

    int geopm_ctl_create(struct geopm_policy_c *policy, MPI_Comm comm, struct geopm_ctl_c **ctl)
    {
        int err = 0;
        try {
            geopm::GlobalPolicy *global_policy = (geopm::GlobalPolicy *)policy;
            *ctl = (struct geopm_ctl_c *)(new geopm::Controller(global_policy, comm));
        }
        catch (...) {
            err = geopm::exception_handler(std::current_exception());
        }
        return err;
    }

    int geopm_ctl_create_f(struct geopm_policy_c *policy, int comm, struct geopm_ctl_c **ctl)
    {
        return geopm_ctl_create(policy, MPI_Comm_f2c(comm), ctl);
    }

    int geopm_ctl_destroy(struct geopm_ctl_c *ctl)
    {
        int err = 0;
        geopm::Controller *ctl_obj = (geopm::Controller *)ctl;
        try {
            delete ctl_obj;
        }
        catch (...) {
            err = geopm::exception_handler(std::current_exception());
        }
        return err;
    }

    int geopm_ctl_run(struct geopm_ctl_c *ctl)
    {
        int err = 0;
        geopm::Controller *ctl_obj = (geopm::Controller *)ctl;
        try {
            ctl_obj->run();
        }
        catch (...) {
            ctl_obj->reset();
            err = geopm::exception_handler(std::current_exception());
        }
        return err;
    }
    int geopm_ctl_step(struct geopm_ctl_c *ctl)
    {
        int err = 0;
        geopm::Controller *ctl_obj = (geopm::Controller *)ctl;
        try {
            ctl_obj->step();
        }
        catch (...) {
            err = geopm::exception_handler(std::current_exception());
        }
        return err;
    }
    int geopm_ctl_pthread(struct geopm_ctl_c *ctl,
                          const pthread_attr_t *attr,
                          pthread_t *thread)
    {
        long err = 0;
        geopm::Controller *ctl_obj = (geopm::Controller *)ctl;
        try {
            ctl_obj->pthread(attr, thread);
        }
        catch (...) {
            err = geopm::exception_handler(std::current_exception());
        }
        return err;
    }
}

namespace geopm
{
    Controller::Controller(GlobalPolicy *global_policy, MPI_Comm comm)
        : m_is_node_root(false)
        , m_max_fanout(0)
        , m_global_policy(global_policy)
        , m_tree_comm(NULL)
        , m_leaf_decider(NULL)
        , m_decider_factory(NULL)
        , m_platform_factory(NULL)
        , m_platform(NULL)
        , m_sampler(NULL)
        , m_sample_regulator(NULL)
        , m_tracer(NULL)
        , m_region_id_all(0)
        , m_do_shutdown(false)
        , m_is_connected(false)
        , m_control_rate_limit(0.0)
        , m_sample_rate_limit(0.005)
        , m_rank_per_node(0)
        , m_epoch_time(0.0)
        , m_mpi_sync_time(0.0)
        , m_mpi_agg_time(0.0)
        , m_sample_count(0)
        , m_throttle_count(0)
        , m_throttle_limit_mhz(0.0)
        , m_app_start_time({{0,0}})
        , m_counter_energy_start(0.0)
        , m_ppn1_comm(MPI_COMM_NULL)
    {
        int err = 0;
        int num_nodes = 0;

        err = geopm_comm_split_ppn1(comm, "ctl", &m_ppn1_comm);
        if (err) {
            throw geopm::Exception("geopm_comm_split_ppn1()", err, __FILE__, __LINE__);
        }
        // Only the root rank on each node will have a fully initialized controller
        if (m_ppn1_comm != MPI_COMM_NULL) {
            m_is_node_root = true;
            struct geopm_plugin_description_s plugin_desc;
            int rank;
            geopm_error_destroy_shmem();
            check_mpi(MPI_Comm_rank(m_ppn1_comm, &rank));
            if (!rank) { // We are the root of the tree
                plugin_desc.tree_decider[NAME_MAX - 1] = '\0';
                plugin_desc.leaf_decider[NAME_MAX - 1] = '\0';
                plugin_desc.platform[NAME_MAX - 1] = '\0';
                switch (m_global_policy->mode()) {
                    case GEOPM_POLICY_MODE_TDP_BALANCE_STATIC:
                    case GEOPM_POLICY_MODE_FREQ_UNIFORM_STATIC:
                    case GEOPM_POLICY_MODE_FREQ_HYBRID_STATIC:
                    case GEOPM_POLICY_MODE_FREQ_UNIFORM_DYNAMIC:
                    case GEOPM_POLICY_MODE_FREQ_HYBRID_DYNAMIC:
                        throw geopm::Exception("Controller::Controller()", GEOPM_ERROR_NOT_IMPLEMENTED, __FILE__, __LINE__);
                        break;
                    case GEOPM_POLICY_MODE_PERF_BALANCE_DYNAMIC:
                        snprintf(plugin_desc.tree_decider, NAME_MAX, "power_balancing");
                        snprintf(plugin_desc.leaf_decider, NAME_MAX, "power_governing");
                        snprintf(plugin_desc.platform, NAME_MAX, "rapl");
                        break;
                    case GEOPM_POLICY_MODE_STATIC:
                        snprintf(plugin_desc.tree_decider, NAME_MAX, "static_policy");
                        snprintf(plugin_desc.leaf_decider, NAME_MAX, "static_policy");
                        snprintf(plugin_desc.platform, NAME_MAX, "rapl");
                        break;
                    case GEOPM_POLICY_MODE_DYNAMIC:
                        strncpy(plugin_desc.tree_decider, m_global_policy->tree_decider().c_str(), NAME_MAX -1);
                        strncpy(plugin_desc.leaf_decider, m_global_policy->leaf_decider().c_str(), NAME_MAX -1);
                        strncpy(plugin_desc.platform, m_global_policy->platform().c_str(), NAME_MAX -1);
                        break;
                    default:
                        throw Exception("Controller::Controller(): Invalid mode", GEOPM_ERROR_INVALID, __FILE__, __LINE__);
                }
            }
            check_mpi(MPI_Bcast(&plugin_desc, sizeof(plugin_desc), MPI_CHAR, 0, m_ppn1_comm));

            check_mpi(MPI_Comm_size(m_ppn1_comm, &num_nodes));

            if (num_nodes > 1) {
                int num_fan_out = 1;
                std::vector<int> fan_out(num_fan_out);
                fan_out[0] = num_nodes;

                while (fan_out[0] > M_MAX_FAN_OUT && fan_out[num_fan_out - 1] != 1) {
                    ++num_fan_out;
                    fan_out.resize(num_fan_out);
                    std::fill(fan_out.begin(), fan_out.end(), 0);
                    check_mpi(MPI_Dims_create(num_nodes, num_fan_out, fan_out.data()));
                }

                if (num_fan_out > 1 && fan_out[num_fan_out - 1] == 1) {
                    --num_fan_out;
                    fan_out.resize(num_fan_out);
                }
                std::reverse(fan_out.begin(), fan_out.end());

                m_tree_comm = new TreeCommunicator(fan_out, global_policy, m_ppn1_comm);
            }
            else {
                m_tree_comm = new SingleTreeCommunicator(global_policy);
            }

            int num_level = m_tree_comm->num_level();
            m_region.resize(num_level);
            m_policy.resize(num_level);
            m_tree_decider.resize(num_level);
            std::fill(m_tree_decider.begin(), m_tree_decider.end(), (Decider *)NULL);
            m_last_policy_msg.resize(num_level);
            std::fill(m_last_policy_msg.begin(), m_last_policy_msg.end(), GEOPM_POLICY_UNKNOWN);
            m_is_epoch_changed.resize(num_level);
            std::fill(m_is_epoch_changed.begin(), m_is_epoch_changed.end(), false);

            m_last_sample_msg.resize(num_level);
            std::fill(m_last_sample_msg.begin(), m_last_sample_msg.end(), GEOPM_SAMPLE_INVALID);

            m_platform_factory = new PlatformFactory;
            m_platform = m_platform_factory->platform(plugin_desc.platform);
            m_msr_sample.resize(m_platform->capacity());
            m_platform->sample(m_msr_sample);
            m_app_start_time = m_msr_sample[0].timestamp;
            for (auto it = m_msr_sample.begin(); it != m_msr_sample.end(); ++it) {
                if ((*it).domain_type == GEOPM_DOMAIN_PACKAGE &&
                    ((*it).signal_type == GEOPM_TELEMETRY_TYPE_DRAM_ENERGY ||
                     (*it).signal_type == GEOPM_TELEMETRY_TYPE_PKG_ENERGY)) {
                    m_counter_energy_start += (*it).signal;
                }
            }
            double upper_bound;
            double lower_bound;
            m_platform->bound(upper_bound, lower_bound);
            m_throttle_limit_mhz = m_platform->throttle_limit_mhz();
            // convert rate limit from ms to seconds
            m_control_rate_limit = m_platform->control_latency_ms() * 1E-3;
            geopm_time(&m_control_loop_t0);
            m_sample_loop_t0 = m_control_loop_t0;

            m_decider_factory = new DeciderFactory;
            m_leaf_decider = m_decider_factory->decider(std::string(plugin_desc.leaf_decider));
            m_leaf_decider->bound(upper_bound, lower_bound);

            int num_domain;
            for (int level = 0; level < num_level; ++level) {
                if (level == 0) {
                    num_domain = m_platform->num_control_domain();
                    m_telemetry_sample.resize(num_domain, {0, {{0, 0}}, {0}});
                }
                else {
                    num_domain = m_tree_comm->level_size(level - 1);
                }
                m_policy[level] = new Policy(num_domain);
                if (m_platform->control_domain() == GEOPM_CONTROL_DOMAIN_POWER && level == 1) {
                    upper_bound *= m_platform->num_control_domain();
                    lower_bound *= m_platform->num_control_domain();
                    if (level > 1) {
                        int i = level - 1;
                        while (i >= 0) {
                            upper_bound *= m_tree_comm->level_size(i);
                            --i;
                        }
                    }
                }
                m_tree_decider[level] = m_decider_factory->decider(std::string(plugin_desc.tree_decider));
                m_tree_decider[level]->bound(upper_bound, lower_bound);
                m_region[level].insert(std::pair<uint64_t, Region *>
                                       (GEOPM_REGION_ID_EPOCH,
                                        new Region(GEOPM_REGION_ID_EPOCH,
                                                   GEOPM_POLICY_HINT_UNKNOWN,
                                                   num_domain,
                                                   level)));
                if (m_tree_comm->level_size(level) > m_max_fanout) {
                    m_max_fanout = m_tree_comm->level_size(level);
                }
            }

            // Synchronize the ranks so time zero is uniform.
            MPI_Barrier(m_ppn1_comm);
            m_tracer = new Tracer();
            m_sampler = new ProfileSampler(M_SHMEM_REGION_SIZE);
        }
    }

    Controller::~Controller()
    {
        if (!m_is_node_root) {
            return;
        }

        m_do_shutdown = true;
        if (m_ppn1_comm != MPI_COMM_NULL) {
            MPI_Barrier(m_ppn1_comm);
            MPI_Comm_free(&m_ppn1_comm);
        }

        delete m_tracer;
        for (int level = 0; level < m_tree_comm->num_level(); ++level) {
            for (auto it = m_region[level].begin(); it != m_region[level].end(); ++it) {
                delete (*it).second;
            }
            delete m_policy[level];
        }
        for (auto it = m_tree_decider.begin(); it != m_tree_decider.end(); ++it) {
            delete (*it);
        }
        delete m_leaf_decider;
        delete m_decider_factory;
        delete m_platform_factory;
        delete m_tree_comm;
        delete m_sampler;
        delete m_sample_regulator;
    }


    void Controller::connect(void)
    {
        if (!m_is_node_root) {
            return;
        }

        if (!m_is_connected) {
            m_sampler->initialize(m_rank_per_node);
            m_num_mpi_enter.resize(m_rank_per_node, 0);
            m_mpi_enter_time.resize(m_rank_per_node, {{0,0}});
            m_prof_sample.resize(m_sampler->capacity());
            std::vector<int> cpu_rank;
            m_sampler->cpu_rank(cpu_rank);
            m_platform->init_transform(cpu_rank);
            m_sample_regulator = new SampleRegulator(cpu_rank);
            m_is_connected = true;
        }
    }

    void Controller::run(void)
    {
        if (!m_is_node_root) {
            return;
        }

        geopm_signal_handler_register();

        connect();

        geopm_signal_handler_check();

        int level;
        int err = 0;
        struct geopm_policy_message_s policy;

        // Spin waiting for for first policy message
        level = m_tree_comm->num_level() - 1;
        do {
            try {
                m_tree_comm->get_policy(level, policy);
                err = 0;
            }
            catch (Exception ex) {
                if (ex.err_value() != GEOPM_ERROR_POLICY_UNKNOWN) {
                    throw ex;
                }
                err = 1;
            }
            geopm_signal_handler_check();
        }
        while (err);

        while (!m_do_shutdown) {
            walk_down();
            geopm_signal_handler_check();
            walk_up();
            geopm_signal_handler_check();
        }
        geopm_signal_handler_check();

        reset();
    }

    void Controller::step(void)
    {
        if (!m_is_node_root) {
            return;
        }

        connect();

        walk_down();
        geopm_signal_handler_check();
        walk_up();
        geopm_signal_handler_check();

        if (m_do_shutdown) {
            throw Exception("Controller::step(): Shutdown signaled", GEOPM_ERROR_SHUTDOWN, __FILE__, __LINE__);
        }
    }

    void Controller::pthread(const pthread_attr_t *attr, pthread_t *thread)
    {
        if (!m_is_node_root) {
            return;
        }

        int err = pthread_create(thread, attr, geopm_threaded_run, (void *)this);
        if (err) {
            throw Exception("Controller::pthread(): pthread_create() failed", err, __FILE__, __LINE__);
        }
    }

    void Controller::walk_down(void)
    {
        int level;
        struct geopm_policy_message_s policy_msg;
        std::vector<struct geopm_policy_message_s> child_policy_msg(m_max_fanout);

        level = m_tree_comm->num_level() - 1;
        m_tree_comm->get_policy(level, policy_msg);
        for (; policy_msg.mode != GEOPM_POLICY_MODE_SHUTDOWN && level != 0; --level) {
            if (!geopm_is_policy_equal(&policy_msg, &(m_last_policy_msg[level]))) {
                m_tree_decider[level]->update_policy(policy_msg, *(m_policy[level]));
                m_policy[level]->policy_message(GEOPM_REGION_ID_EPOCH, policy_msg, child_policy_msg);
                m_tree_comm->send_policy(level - 1, child_policy_msg);
                m_last_policy_msg[level] = policy_msg;
            }
            m_tree_comm->get_policy(level - 1, policy_msg);
        }
        if (policy_msg.mode == GEOPM_POLICY_MODE_SHUTDOWN) {
            m_do_shutdown = true;
        }
        else {
            // update the leaf level (0)
            if (!geopm_is_policy_equal(&policy_msg, &(m_last_policy_msg[level]))) {
                m_leaf_decider->update_policy(policy_msg, *(m_policy[level]));
                m_last_policy_msg[level] = policy_msg;
                m_tracer->update(policy_msg);
            }
        }
    }

    void Controller::walk_up(void)
    {
        int level;
        std::vector<struct geopm_sample_message_s> child_sample(m_max_fanout);
        std::vector<struct geopm_policy_message_s> child_policy_msg(m_max_fanout);
        std::vector<struct geopm_sample_message_s> sample_msg(m_tree_comm->num_level());
        std::vector<struct geopm_telemetry_message_s> epoch_telemetry_sample(m_telemetry_sample.size());
        size_t length;
        struct geopm_time_s sample_loop_t1;

        std::fill(sample_msg.begin(), sample_msg.end(), GEOPM_SAMPLE_INVALID);
        do {
            geopm_time(&sample_loop_t1);
            geopm_signal_handler_check();
        }
        while (geopm_time_diff(&m_sample_loop_t0, &sample_loop_t1) < m_sample_rate_limit);
        m_sample_loop_t0 = sample_loop_t1;

        for (level = 0; !m_do_shutdown && level < m_tree_comm->num_level(); ++level) {
            bool is_converged = false;
            if (level) {
                try {
                    m_tree_comm->get_sample(level, child_sample);
                    // use .begin() because map has only one entry
                    auto it = m_region[level].begin();
                    (*it).second->insert(child_sample);
                    if (m_tree_decider[level]->update_policy(*((*it).second), *(m_policy[level]))) {
                        m_policy[level]->policy_message(GEOPM_REGION_ID_EPOCH, m_last_policy_msg[level], child_policy_msg);
                        m_tree_comm->send_policy(level - 1, child_policy_msg);
                    }
                    (*it).second->sample_message(sample_msg[level]);
                    if (sample_msg[level].signal[GEOPM_SAMPLE_TYPE_RUNTIME] != m_last_sample_msg[level].signal[GEOPM_SAMPLE_TYPE_RUNTIME]) {
                        m_is_epoch_changed[level] = true;
                    }
                    is_converged = m_policy[level]->is_converged(GEOPM_REGION_ID_EPOCH);
                }
                catch (geopm::Exception ex) {
                    if (ex.err_value() != GEOPM_ERROR_SAMPLE_INCOMPLETE) {
                        throw ex;
                    }
                    break;
                }
            }
            else {
                // Sample from the application, sample from RAPL,
                // sample from the MSRs and fuse all this data into a
                // single sample using coherant time stamps to
                // calculate elapsed values. We then pass this to the
                // decider who will create a new per domain policy for
                // the current region. Then we can enforce the policy
                // by adjusting RAPL power domain limits.
                m_sampler->sample(m_prof_sample, length, m_ppn1_comm);

                // Find all MPI time and aggregate.
                for (auto sample_it = m_prof_sample.begin();
                     sample_it != m_prof_sample.begin() + length;
                     ++sample_it) {
                    // Use the map from the sample regulator get the node local rank index for the sample.
                    int local_rank = (*(m_sample_regulator->rank_idx_map().find((*sample_it).second.rank))).second;
                    if (geopm_region_id_is_mpi((*sample_it).second.region_id)) {
                        if ((*sample_it).second.progress == 0.0) {
                            if (!m_num_mpi_enter[local_rank]) {
                                m_mpi_enter_time[local_rank] = (*sample_it).second.timestamp;
                            }
                            ++m_num_mpi_enter[local_rank];
                        }
                        else if ((*sample_it).second.progress == 1.0) {
                            --m_num_mpi_enter[local_rank];
                            if (!m_num_mpi_enter[local_rank]) {
                                m_mpi_sync_time += geopm_time_diff(&(m_mpi_enter_time[local_rank]), &((*sample_it).second.timestamp)) / m_rank_per_node;
                            }
                        }
                        (*sample_it).second.region_id = GEOPM_REGION_ID_INVALID;
                    }
                }

                m_platform->sample(m_msr_sample);
                // Insert MSR data into platform sample
                std::vector<double> platform_sample(m_msr_sample.size());
                auto output_it = platform_sample.begin();
                for (auto input_it = m_msr_sample.begin(); input_it != m_msr_sample.end(); ++input_it) {
                    *output_it = (*input_it).signal;
                    ++output_it;
                }

                std::vector<double> aligned_signal;
                bool is_epoch_found = false;
                // Catch epoch regions and region entries
                for (auto sample_it = m_prof_sample.cbegin();
                     sample_it != m_prof_sample.cbegin() + length;
                     ++sample_it) {
                    if ((*sample_it).second.region_id != GEOPM_REGION_ID_INVALID) {
                        if ((*sample_it).second.progress == 0.0) {
                            auto region_it = m_region[level].find((*sample_it).second.region_id);
                            if (region_it == m_region[level].end()) {
                                auto tmp_it = m_region[level].insert(
                                                  std::pair<uint64_t, Region *> ((*sample_it).second.region_id,
                                                          new Region((*sample_it).second.region_id,
                                                                     GEOPM_POLICY_HINT_UNKNOWN,
                                                                     m_platform->num_control_domain(),
                                                                     level)));
                                region_it = tmp_it.first;
                            }
                            (*region_it).second->entry();
                        }
                        if (!is_epoch_found &&
                            geopm_region_id_is_epoch((*sample_it).second.region_id)) {
                            uint64_t region_id_all_tmp = m_region_id_all;
                            m_region_id_all = GEOPM_REGION_ID_EPOCH;
                            (*m_sample_regulator)(m_msr_sample[0].timestamp,
                                                  platform_sample.cbegin(), platform_sample.cend(),
                                                  m_prof_sample.cbegin(), m_prof_sample.cbegin(), // Not using profile data with epoch samples
                                                  aligned_signal,
                                                  m_region_id);
                            m_platform->transform_rank_data(m_region_id_all, m_msr_sample[0].timestamp, aligned_signal, epoch_telemetry_sample);
                            is_epoch_found = true;
                            m_region_id_all = region_id_all_tmp;
                        }
                    }
                }

                // Align profile data
                (*m_sample_regulator)(m_msr_sample[0].timestamp,
                                      platform_sample.cbegin(), platform_sample.cend(),
                                      m_prof_sample.cbegin(), m_prof_sample.cbegin() + length,
                                      aligned_signal,
                                      m_region_id);

                // Determine if all ranks were last sampled from the same region
                uint64_t region_id_all = m_region_id[0];
                for (auto it = m_region_id.begin(); it != m_region_id.end(); ++it) {
                    if (*it != region_id_all) {
                        region_id_all = 0;
                    }
                }

                m_platform->transform_rank_data(region_id_all, m_msr_sample[0].timestamp, aligned_signal, m_telemetry_sample);

                // Exiting a region for unmarked code
                if (m_region_id_all && !region_id_all) {
                    override_telemetry(1.0);
                    update_region();
                    m_region_id_all = 0;
                    std::fill(m_region_id.begin(), m_region_id.end(), 0);
                    if (is_epoch_found) {
                        update_epoch(epoch_telemetry_sample);
                    }
                }
                // Entering a region from unmarked code
                else if (!m_region_id_all && region_id_all) {
                    if (is_epoch_found) {
                        update_epoch(epoch_telemetry_sample);
                    }
                    m_region_id_all = region_id_all;
                    override_telemetry(0.0);
                    update_region();
                }
                // Entering a region from another region
                else if (m_region_id_all && region_id_all &&
                         m_region_id_all != region_id_all) {
                    override_telemetry(1.0);
                    update_region();
                    if (is_epoch_found) {
                        update_epoch(epoch_telemetry_sample);
                    }
                    m_region_id_all = region_id_all;
                    override_telemetry(0.0);
                    std::fill(m_region_id.begin(), m_region_id.end(), region_id_all);
                    update_region();
                }
                // No entries or exits
                else {
                    if (is_epoch_found) {
                        update_epoch(epoch_telemetry_sample);
                    }
                    update_region();
                }
                // GEOPM_REGION_ID_EPOCH is inserted at construction
                struct geopm_sample_message_s epoch_sample;
                auto epoch_it = m_region[level].find(GEOPM_REGION_ID_EPOCH);
                (*epoch_it).second->sample_message(epoch_sample);
                for (auto it = m_region[level].begin(); it != m_region[level].end(); ++it) {
                    if (m_policy[level]->is_converged(((*it).first))) {
                        is_converged = true;
                    }
                }
                // Subtract mpi syncronization time from epoch
                if (epoch_sample.signal[GEOPM_SAMPLE_TYPE_RUNTIME] != m_epoch_time &&
                    is_converged) {
                    sample_msg[level] = epoch_sample;
                    m_epoch_time = sample_msg[level].signal[GEOPM_SAMPLE_TYPE_RUNTIME];
                    m_is_epoch_changed[level] = true;
                    sample_msg[level].signal[GEOPM_SAMPLE_TYPE_RUNTIME] -= m_mpi_sync_time;
                }
                if (is_epoch_found) {
                    m_mpi_agg_time += m_mpi_sync_time;
                    m_mpi_sync_time = 0.0;
                }
                m_do_shutdown = m_sampler->do_shutdown();
            }
            if (level != m_tree_comm->root_level() &&
                is_converged &&
                m_is_epoch_changed[level]) {
                m_tree_comm->send_sample(level, sample_msg[level]);
                m_last_sample_msg[level] = sample_msg[level];
                m_is_epoch_changed[level] = false;
            }
        }
        if (m_do_shutdown) {
            MPI_Barrier(m_ppn1_comm);
            if (m_sampler->do_report()) {
                generate_report();
            }
        }
    }

    void Controller::update_epoch(std::vector<struct geopm_telemetry_message_s> &telemetry)
    {
        static bool is_in_epoch = false;
        uint64_t region_id_all_tmp = m_region_id_all;
        m_region_id_all = GEOPM_REGION_ID_EPOCH;
        m_telemetry_sample.swap(telemetry);
        if (is_in_epoch) {
            override_telemetry(1.0);
            update_region();
        }
        is_in_epoch = true;
        override_telemetry(0.0);
        update_region();
        m_region_id_all = region_id_all_tmp;
        m_telemetry_sample.swap(telemetry);
    }

    void Controller::override_telemetry(double progress)
    {
        for (auto it = m_telemetry_sample.begin(); it != m_telemetry_sample.end(); ++it) {
            (*it).region_id = m_region_id_all;
            (*it).signal[GEOPM_TELEMETRY_TYPE_PROGRESS] = progress;
            (*it).signal[GEOPM_TELEMETRY_TYPE_RUNTIME] = 0.0;
        }
    }

    void Controller::update_region(void)
    {
        struct geopm_time_s control_loop_t1;

        m_tracer->update(m_telemetry_sample);
        m_sample_count++;
        if (m_telemetry_sample[0].signal[GEOPM_TELEMETRY_TYPE_FREQUENCY] <= m_throttle_limit_mhz) {
            m_throttle_count++;
        }

        if (m_region_id_all) {
            int level = 0; // Called only at the leaf
            auto it = m_region[level].find(m_region_id_all);
            if (it == m_region[level].end()) {
                auto tmp_it = m_region[level].insert(
                                  std::pair<uint64_t, Region *> (m_region_id_all,
                                          new Region(m_region_id_all,
                                                     GEOPM_POLICY_HINT_UNKNOWN,
                                                     m_platform->num_control_domain(),
                                                     level)));
                it = tmp_it.first;
            }
            Region *curr_region = (*it).second;
            Policy *curr_policy = m_policy[level];
            curr_region->insert(m_telemetry_sample);

            geopm_time(&control_loop_t1);
            if (geopm_time_diff(&m_control_loop_t0, &control_loop_t1) >= m_control_rate_limit &&
                !geopm_region_id_is_epoch(m_region_id_all) &&
                m_leaf_decider->update_policy(*curr_region, *curr_policy) == true) {
                m_platform->enforce_policy(m_region_id_all, *curr_policy);
                m_control_loop_t0 = control_loop_t1;
            }
        }
    }

    void Controller::generate_report(void)
    {
        if (!m_is_node_root || !m_is_connected) {
            return;
        }

        m_mpi_agg_time += m_mpi_sync_time;
        if ((*(m_region[0].find(GEOPM_REGION_ID_EPOCH))).second->num_entry()) {
            m_region_id_all = GEOPM_REGION_ID_EPOCH;
            override_telemetry(1.0);
            update_region();
        }

        std::string report_name;
        std::string profile_name;
        std::set<std::string> region_name;
        std::map<uint64_t, std::string> region;
        std::ofstream report;
        char hostname[NAME_MAX];
        double energy_exit = 0.0;

        m_platform->sample(m_msr_sample);
        for (auto it = m_msr_sample.begin(); it != m_msr_sample.end(); ++it) {
            if ((*it).domain_type == GEOPM_DOMAIN_PACKAGE &&
                ((*it).signal_type == GEOPM_TELEMETRY_TYPE_DRAM_ENERGY ||
                 (*it).signal_type == GEOPM_TELEMETRY_TYPE_PKG_ENERGY)) {
                energy_exit += (*it).signal;
            }
        }
        m_sampler->report_name(report_name);
        m_sampler->profile_name(profile_name);
        m_sampler->name_set(region_name);

        if (report_name.empty() || profile_name.empty()) {
            throw Exception("Controller::generate_report(): Invalid report data", GEOPM_ERROR_INVALID, __FILE__, __LINE__);
        }

        // create a map from region_id to name
        for (auto it = region_name.begin(); it != region_name.end(); ++it) {
            uint64_t region_id = geopm_crc32_str(0, (*it).c_str());
            region.insert(std::pair<uint64_t, std::string>(region_id, (*it)));
        }

        gethostname(hostname, NAME_MAX);
        std::ostringstream host_report_name;
        host_report_name << report_name << "-" << hostname;
        report.open(host_report_name.str(), std::ios_base::out);
        report << "##### geopm " << geopm_version() << " #####" << std::endl << std::endl;
        report << "Profile: " << profile_name << std::endl;
        for (auto it = m_region[0].begin(); it != m_region[0].end(); ++it) {
            uint64_t region_id = (*it).first;
            std::string name;
            if (region_id == GEOPM_REGION_ID_EPOCH) {
                name = "epoch";
            }
            else if (region_id == 0) {
                name = "unmarked region";
            }
            else {
                auto region_it = region.find(region_id);
                if (region_it != region.end()) {
                    name = (*region_it).second;
                }
                else {
                    std::ostringstream ex_str;
                    ex_str << "Controller::generate_report(): Invalid region " << region_id;
                    throw Exception(ex_str.str(), GEOPM_ERROR_INVALID, __FILE__, __LINE__);
                }
            }
            if ((*it).second->num_entry()) {
                (*it).second->report(report, name, m_rank_per_node);
            }
        }
        double total_runtime = geopm_time_diff(&m_app_start_time, &m_msr_sample[0].timestamp);
        report << "Application Totals:" << std::endl
               << "\truntime (sec): " << total_runtime << std::endl
               << "\tenergy (joules): " << (energy_exit - m_counter_energy_start) << std::endl
               << "\tmpi-runtime (sec): " << m_mpi_agg_time << std::endl
               << "\tthrottle time (%): " << (double)m_throttle_count / (double)m_sample_count * 100.0 << std::endl;

        std::ostringstream proc_str;
        proc_str << "/proc/" << (int)getpid() <<  "/status";
        std::ifstream proc_stream(proc_str.str());
        std::string line;
        std::string max_memory;
        const std::string key("VmHWM:");
        while (proc_stream.good()) {
            getline(proc_stream, line);
            if (line.find(key) == 0) {
                max_memory = line.substr(key.length());
                size_t off = max_memory.find_first_not_of(" \t");
                if (off != std::string::npos) {
                    max_memory = max_memory.substr(off);
                }
            }
        }
        proc_stream.close();
        if (!max_memory.size()) {
            throw Exception("Controller::generate_report(): Unable to get memory overhead from /proc",
                            GEOPM_ERROR_RUNTIME, __FILE__, __LINE__);
        }
        report << "\tgeopmctl memory HWM: " << max_memory << std::endl;
        report << "\tgeopmctl network BW (B/sec): " << m_tree_comm->overhead_send() / total_runtime << std::endl;

        report.close();
    }

    void Controller::reset(void)
    {
        geopm_error_destroy_shmem();
        m_platform->revert_msr_state();
    }
}
