set(srcs "bwg_authority.c" "bwg_limits.c" "bwg_link_presence.c" "bwg_provision.c" "bwg_revocation.c" "bwg_v2_runtime.c"
         "proto/bwg_alloc.c" "proto/bwg_codec.c" "proto/bwg_controller.c" "proto/bwg_controller_commands.c" "proto/bwg_controller_cooling.c" "proto/bwg_controller_evidence.c" "proto/bwg_evidence_lines.c" "proto/bwg_trace.c" "proto/bwg_cadence.c" "proto/bwg_v2_record.c" "proto/bwg_v2_write.c" "proto/bwg_controller_v2.c" "proto/bwg_noise_record.c" "proto/bwg_controller_noise.c" "proto/bwg_controller_reviews.c"
         "proto/bwg_controller_wire.c" "proto/bwg_crash_streak.c" "proto/bwg_frame.c" "proto/bwg_json.c" "proto/bwg_json_writer.c"
         "proto/bwg_lease.c" "proto/bwg_lease_auth.c" "proto/bwg_link.c" "proto/bwg_link_shared.c"
         "proto/bwg_allowance.c" "proto/bwg_budget.c" "proto/bwg_ledger.c" "proto/bwg_possession.c" "proto/bwg_progress.c" "proto/bwg_reason.c" "proto/bwg_review.c" "proto/bwg_scan.c" "proto/bwg_seq_doc.c"
         "proto/bwg_trust.c" "proto/bwg_writer.c" "proto/bwg_cutoff.c" "proto/bwg_core_dump_receipt.c" "proto/bwg_preparation_receipt.c" "proto/bwg_restart_guard.c")
if(CONFIG_BWG_REVOCATION_PROOF)
    list(APPEND srcs "bwg_revocation_proof.c")
endif()

# A Gate build names a private directory holding the deployment trust and signed capabilities. Without
# one the image takes no part in the Gate. Such an image holds no secret, but its trust is deployment
# specific, so keep it with the owner's other images.
set(embed "")
set(trust_dir "$ENV{BWG_TRUST_DIR}")
if(trust_dir)
    set(embed "${trust_dir}/trust.json" "${trust_dir}/capability-205.json" "${trust_dir}/capability-601.json")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include" "proto/include"
                       REQUIRES espressif__libsodium yyjson mining_job
                       PRIV_REQUIRES esp_timer lwip
                       EMBED_TXTFILES ${embed})

# Only a provisioned image reports its commit, and a source archive has none to report.
if(trust_dir)
    execute_process(COMMAND git rev-parse HEAD
                    WORKING_DIRECTORY "${COMPONENT_DIR}"
                    OUTPUT_VARIABLE firmware_commit
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND git rev-parse --path-format=absolute --git-path logs/HEAD
                    WORKING_DIRECTORY "${COMPONENT_DIR}"
                    OUTPUT_VARIABLE head_log
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    COMMAND_ERROR_IS_FATAL ANY)
    # A new commit has to reach the image, which reports it as its source.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${head_log}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BWG_PROVISIONED=1)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE "BWG_FIRMWARE_COMMIT=\"${firmware_commit}\"")
