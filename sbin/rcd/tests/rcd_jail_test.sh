#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Baptiste Daroussin <bapt@FreeBSD.org>
#
# Integration tests for rcd(8) running inside a jail.
# Each test creates its own lightweight jail with nullfs + tmpfs,
# populates it with test-specific units, and verifies behavior.
#

. $(atf_get_srcdir)/rcd_utils.subr

# ---------------------------------------------------------------
# Test: rcd boots in a jail and creates the control socket
# ---------------------------------------------------------------
atf_test_case boot_in_jail cleanup
boot_in_jail_head()
{
	atf_set descr "rcd boots successfully in a jail"
	atf_set require.user root
	atf_set timeout 60
}

boot_in_jail_body()
{
	rcd_init

	rcd_mkjail "boot"

	# Add a simple barrier so rcd has something to process
	rcd_add_unit "boot" "test_barrier.ucl" <<-EOF
	name = "TEST_BOOT";
	type = "barrier";
	provides = ["TEST_BOOT"];
	enable = true;
	EOF

	rcd_start "boot"
	rcd_wait "boot"

	# Control socket must exist
	atf_check -s exit:0 \
	    test -S "/var/tmp/rcd_jail_boot.$$/var/run/rcd.sock"

	# rcd process must be running
	atf_check -s exit:0 -o match:"rcd" \
	    jexec "boot" ps aux
}

boot_in_jail_cleanup()
{
	rcd_cleanup
}

# ---------------------------------------------------------------
# Test: rcctl status works with native units
# ---------------------------------------------------------------
atf_test_case rcctl_status cleanup
rcctl_status_head()
{
	atf_set descr "rcctl status shows test services"
	atf_set require.user root
	atf_set timeout 60
}

rcctl_status_body()
{
	rcd_init

	rcd_mkjail "status"

	rcd_add_unit "status" "mybarrier.ucl" <<-EOF
	name = "MYBARRIER";
	type = "barrier";
	provides = ["MYBARRIER"];
	enable = true;
	EOF

	rcd_add_unit "status" "myoneshot.ucl" <<-EOF
	name = "myoneshot";
	type = "oneshot";
	command = "/usr/bin/true";
	provides = ["myoneshot"];
	enable = true;
	requires = ["MYBARRIER"];
	EOF

	rcd_start "status"
	rcd_wait "status"

	# rcctl status should show our services
	atf_check -s exit:0 -o match:"SERVICE" \
	    jexec "status" /sbin/rcctl status
	atf_check -s exit:0 -o match:"MYBARRIER" \
	    jexec "status" /sbin/rcctl status
	atf_check -s exit:0 -o match:"myoneshot" \
	    jexec "status" /sbin/rcctl status
}

rcctl_status_cleanup()
{
	rcd_cleanup
}

# ---------------------------------------------------------------
# Test: barriers reach done state
# ---------------------------------------------------------------
atf_test_case barriers_done cleanup
barriers_done_head()
{
	atf_set descr "Barrier units reach done state"
	atf_set require.user root
	atf_set timeout 60
}

barriers_done_body()
{
	rcd_init

	rcd_mkjail "barriers"

	rcd_add_unit "barriers" "PHASE1.ucl" <<-EOF
	name = "PHASE1";
	type = "barrier";
	provides = ["PHASE1"];
	enable = true;
	EOF

	rcd_add_unit "barriers" "PHASE2.ucl" <<-EOF
	name = "PHASE2";
	type = "barrier";
	provides = ["PHASE2"];
	enable = true;
	requires = ["PHASE1"];
	EOF

	rcd_start "barriers"
	rcd_wait "barriers"

	atf_check -s exit:0 -o match:"PHASE1.*done.*barrier" \
	    jexec "barriers" /sbin/rcctl status
	atf_check -s exit:0 -o match:"PHASE2.*done.*barrier" \
	    jexec "barriers" /sbin/rcctl status
}

barriers_done_cleanup()
{
	rcd_cleanup
}

# ---------------------------------------------------------------
# Test: oneshot service runs and completes
# ---------------------------------------------------------------
atf_test_case oneshot_completes cleanup
oneshot_completes_head()
{
	atf_set descr "Oneshot service runs to completion"
	atf_set require.user root
	atf_set timeout 60
}

oneshot_completes_body()
{
	rcd_init

	rcd_mkjail "oneshot"

	rcd_add_unit "oneshot" "marker.ucl" <<-EOF
	name = "marker";
	type = "oneshot";
	command = "/usr/bin/touch";
	command_args = "/var/tmp/marker_done";
	provides = ["marker"];
	enable = true;
	EOF

	rcd_start "oneshot"
	rcd_wait "oneshot"

	# Wait for oneshot to complete
	sleep 2

	# The marker file should exist
	atf_check -s exit:0 \
	    jexec "oneshot" test -f /var/tmp/marker_done

	# Service should show done
	atf_check -s exit:0 -o match:"marker.*done" \
	    jexec "oneshot" /sbin/rcctl status
}

oneshot_completes_cleanup()
{
	rcd_cleanup
}

# ---------------------------------------------------------------
# Test: nojail keyword hides service
# ---------------------------------------------------------------
atf_test_case nojail_status cleanup
nojail_status_head()
{
	atf_set descr "nojail keyword prevents service from starting in jail"
	atf_set require.user root
	atf_set timeout 60
}

nojail_status_body()
{
	rcd_init

	rcd_mkjail "nojail"

	rcd_add_unit "nojail" "hostonly.ucl" <<-EOF
	name = "hostonly";
	type = "oneshot";
	command = "/usr/bin/true";
	provides = ["hostonly"];
	enable = true;
	keywords = ["nojail"];
	EOF

	rcd_start "nojail"
	rcd_wait "nojail"

	# Service should show as nojail, not done
	atf_check -s exit:0 -o match:"hostonly.*nojail" \
	    jexec "nojail" /sbin/rcctl status
}

nojail_status_cleanup()
{
	rcd_cleanup
}

# ---------------------------------------------------------------
# Test: rcctl stop/start works for a native oneshot
# ---------------------------------------------------------------
atf_test_case stop_start cleanup
stop_start_head()
{
	atf_set descr "rcctl start re-runs a completed oneshot"
	atf_set require.user root
	atf_set timeout 60
}

stop_start_body()
{
	rcd_init

	rcd_mkjail "stopstart"

	rcd_add_unit "stopstart" "touchfile.ucl" <<-EOF
	name = "touchfile";
	type = "oneshot";
	command = "/usr/bin/touch";
	command_args = "/var/tmp/started";
	provides = ["touchfile"];
	enable = true;
	EOF

	rcd_start "stopstart"
	rcd_wait "stopstart"

	# Wait for initial boot to complete
	sleep 2

	# Should be done after boot
	atf_check -s exit:0 -o match:"touchfile.*done" \
	    jexec "stopstart" /sbin/rcctl status

	# Remove the marker and re-start
	jexec "stopstart" rm -f /var/tmp/started

	atf_check -s exit:0 \
	    jexec "stopstart" /sbin/rcctl start touchfile

	# Marker should exist again
	atf_check -s exit:0 \
	    jexec "stopstart" test -f /var/tmp/started
}

stop_start_cleanup()
{
	rcd_cleanup
}

# ---------------------------------------------------------------
# Test: legacy rc.d script wrapping
# ---------------------------------------------------------------
atf_test_case legacy_wrap cleanup
legacy_wrap_head()
{
	atf_set descr "Legacy rc.d scripts are wrapped as units"
	atf_set require.user root
	atf_set timeout 60
}

legacy_wrap_body()
{
	rcd_init

	rcd_mkjail "legacy"

	rcd_add_legacy "legacy" "test_legacy" <<-'SCRIPT'
#!/bin/sh
#
# PROVIDE: test_legacy
# REQUIRE:
# BEFORE:
# KEYWORD:

. /etc/rc.subr

name="test_legacy"
rcvar="test_legacy_enable"
start_cmd="echo started > /var/tmp/legacy_started"
stop_cmd=":"

load_rc_config $name
run_rc_command "$1"
SCRIPT

	# Enable the legacy service via rc.conf
	echo 'test_legacy_enable="YES"' > \
	    "/var/tmp/rcd_jail_legacy.$$/etc/rc.conf"

	rcd_start "legacy"
	rcd_wait "legacy"

	# Wait for services to complete
	sleep 3

	# The legacy service should appear in status
	atf_check -s exit:0 -o match:"test_legacy" \
	    jexec "legacy" /sbin/rcctl status
}

legacy_wrap_cleanup()
{
	rcd_cleanup
}

# ---------------------------------------------------------------
# Test: disabled service shows correct status
# ---------------------------------------------------------------
atf_test_case disabled_status cleanup
disabled_status_head()
{
	atf_set descr "Disabled services show disabled status"
	atf_set require.user root
	atf_set timeout 60
}

disabled_status_body()
{
	rcd_init

	rcd_mkjail "disabled"

	rcd_add_unit "disabled" "off_svc.ucl" <<-EOF
	name = "off_svc";
	type = "oneshot";
	command = "/usr/bin/true";
	provides = ["off_svc"];
	enable = false;
	EOF

	rcd_start "disabled"
	rcd_wait "disabled"

	# Query by name — disabled services may not appear in the global list
	atf_check -s exit:0 -o match:"off_svc.*disabled" \
	    jexec "disabled" /sbin/rcctl status off_svc
}

disabled_status_cleanup()
{
	rcd_cleanup
}

# ---------------------------------------------------------------
# Test: safe upgrade via SIGUSR1
# ---------------------------------------------------------------
atf_test_case safe_upgrade cleanup
safe_upgrade_head()
{
	atf_set descr "SIGUSR1 triggers safe in-place upgrade"
	atf_set require.user root
	atf_set timeout 60
}

safe_upgrade_body()
{
	rcd_init

	rcd_mkjail "upgrade"

	rcd_add_unit "upgrade" "persistent.ucl" <<-EOF
	name = "persistent";
	type = "simple";
	command = "/bin/sleep";
	command_args = "3600";
	provides = ["persistent"];
	enable = true;
	EOF

	rcd_start "upgrade"
	rcd_wait "upgrade"

	# Wait for service to start
	sleep 2

	# Get rcd PID
	local rcd_pid
	rcd_pid=$(jexec "upgrade" ps -axo pid,comm | \
	    awk '/rcd$/{print $1; exit}')
	if [ -z "$rcd_pid" ]; then
		atf_fail "cannot find rcd PID"
	fi

	# Service should be running
	atf_check -s exit:0 -o match:"persistent.*running" \
	    jexec "upgrade" /sbin/rcctl status

	# Send SIGUSR1 to trigger upgrade — remove old socket first
	# so rcd_wait detects the NEW socket from the re-exec'd rcd.
	_root="/var/tmp/rcd_jail_upgrade.$$"
	rm -f "$_root/var/run/rcd.sock"
	jexec "upgrade" kill -USR1 "$rcd_pid"

	# Wait for the new rcd to re-create the control socket
	rcd_wait "upgrade"

	# Control socket should still work
	atf_check -s exit:0 -o match:"SERVICE" \
	    jexec "upgrade" /sbin/rcctl status

	# The running service should still be tracked
	atf_check -s exit:0 -o match:"persistent.*running" \
	    jexec "upgrade" /sbin/rcctl status
}

safe_upgrade_cleanup()
{
	rcd_cleanup
}

# ---------------------------------------------------------------
# Test: dependency ordering
# ---------------------------------------------------------------
atf_test_case dep_ordering cleanup
dep_ordering_head()
{
	atf_set descr "Services start in dependency order"
	atf_set require.user root
	atf_set timeout 60
}

dep_ordering_body()
{
	rcd_init

	rcd_mkjail "deps"

	# first runs before second (second requires first)
	rcd_add_unit "deps" "first.ucl" <<-EOF
	name = "first";
	type = "oneshot";
	command = "/bin/sh";
	command_args = "-c 'date +%s%N > /var/tmp/first_ts'";
	provides = ["first"];
	enable = true;
	EOF

	rcd_add_unit "deps" "second.ucl" <<-EOF
	name = "second";
	type = "oneshot";
	command = "/bin/sh";
	command_args = "-c 'date +%s%N > /var/tmp/second_ts'";
	provides = ["second"];
	enable = true;
	requires = ["first"];
	EOF

	rcd_start "deps"
	rcd_wait "deps"

	sleep 3

	# Both should be done
	atf_check -s exit:0 -o match:"first.*done" \
	    jexec "deps" /sbin/rcctl status
	atf_check -s exit:0 -o match:"second.*done" \
	    jexec "deps" /sbin/rcctl status

	# first timestamp should be <= second timestamp
	local ts1 ts2
	ts1=$(jexec "deps" cat /var/tmp/first_ts)
	ts2=$(jexec "deps" cat /var/tmp/second_ts)
	if [ -n "$ts1" ] && [ -n "$ts2" ]; then
		atf_check -s exit:0 test "$ts1" -le "$ts2"
	fi
}

dep_ordering_cleanup()
{
	rcd_cleanup
}

# ---------------------------------------------------------------
# Test: restart on-failure restarts a crashed daemon
# ---------------------------------------------------------------
atf_test_case restart_on_failure cleanup
restart_on_failure_head()
{
	atf_set descr "Daemon that exits non-zero is restarted by on-failure policy"
	atf_set require.user root
	atf_set timeout 60
}

restart_on_failure_body()
{
	rcd_init

	rcd_mkjail "restart"

	# Daemon that exits with code 1 on first run, then stays alive.
	# Uses a marker file to track attempts.
	rcd_add_unit "restart" "crasher.ucl" <<-EOF
	name = "crasher";
	type = "simple";
	command = "/bin/sh";
	command_args = "-c 'if [ ! -f /var/tmp/attempt2 ]; then touch /var/tmp/attempt2; exit 1; fi; exec /bin/sleep 3600'";
	provides = ["crasher"];
	enable = true;
	restart {
	    policy = "on-failure";
	    max_retries = 3;
	    delay = 1000;
	}
	EOF

	rcd_start "restart"
	rcd_wait "restart"

	# Wait for the first crash + restart delay + second spawn
	sleep 5

	# The service should be running (second attempt succeeds)
	atf_check -s exit:0 -o match:"crasher.*running" \
	    jexec "restart" /sbin/rcctl status

	# The marker file should exist (proves first attempt happened)
	atf_check -s exit:0 \
	    jexec "restart" test -f /var/tmp/attempt2
}

restart_on_failure_cleanup()
{
	rcd_cleanup
}

# ---------------------------------------------------------------
# Test: start_precmd failure aborts service start
# ---------------------------------------------------------------
atf_test_case precmd_abort cleanup
precmd_abort_head()
{
	atf_set descr "start_precmd failure prevents service from starting"
	atf_set require.user root
	atf_set timeout 60
}

precmd_abort_body()
{
	rcd_init

	rcd_mkjail "precmd"

	rcd_add_unit "precmd" "guarded.ucl" <<-EOF
	name = "guarded";
	type = "oneshot";
	command = "/usr/bin/touch";
	command_args = "/var/tmp/should_not_exist";
	provides = ["guarded"];
	enable = true;
	start_precmd = "/usr/bin/false";
	EOF

	rcd_start "precmd"
	rcd_wait "precmd"

	sleep 2

	# The command should NOT have run (precmd returned non-zero)
	atf_check -s exit:1 \
	    jexec "precmd" test -f /var/tmp/should_not_exist

	# The service should show failed, not done
	atf_check -s exit:0 -o match:"guarded.*failed" \
	    jexec "precmd" /sbin/rcctl status guarded
}

precmd_abort_cleanup()
{
	rcd_cleanup
}

# ---------------------------------------------------------------
# Test: rcctl enable/disable persists across rcd restart
# ---------------------------------------------------------------
atf_test_case enable_disable_persist cleanup
enable_disable_persist_head()
{
	atf_set descr "rcctl enable/disable writes persistent override"
	atf_set require.user root
	atf_set timeout 60
}

enable_disable_persist_body()
{
	rcd_init

	rcd_mkjail "persist"

	rcd_add_unit "persist" "mysvc.ucl" <<-EOF
	name = "mysvc";
	type = "oneshot";
	command = "/usr/bin/true";
	provides = ["mysvc"];
	enable = false;
	EOF

	rcd_start "persist"
	rcd_wait "persist"

	# Service is disabled
	atf_check -s exit:0 -o match:"mysvc.*disabled" \
	    jexec "persist" /sbin/rcctl status mysvc

	# Enable it
	atf_check -s exit:0 \
	    jexec "persist" /sbin/rcctl enable mysvc

	# Override file should exist in the jail
	_root="/var/tmp/rcd_jail_persist.$$"
	atf_check -s exit:0 test -f "$_root/etc/rcd.conf.d/mysvc"

	# Disable it
	atf_check -s exit:0 \
	    jexec "persist" /sbin/rcctl disable mysvc

	# Verify override file has enable=false
	atf_check -s exit:0 -o match:"false" \
	    jexec "persist" cat /etc/rcd.conf.d/mysvc
}

enable_disable_persist_cleanup()
{
	rcd_cleanup
}

# ---------------------------------------------------------------
# Test: process.user drops credentials via rcd-exec
# ---------------------------------------------------------------
atf_test_case rcdexec_user_drop cleanup
rcdexec_user_drop_head()
{
	atf_set descr "rcd-exec drops privileges to the configured user"
	atf_set require.user root
	atf_set timeout 60
}

rcdexec_user_drop_body()
{
	rcd_init

	rcd_mkjail "userdrop"

	rcd_add_unit "userdrop" "whoami_svc.ucl" <<-EOF
	name = "whoami_svc";
	type = "oneshot";
	command = "/bin/sh";
	command_args = "-c '/usr/bin/id -un > /var/tmp/whoami'";
	provides = ["whoami_svc"];
	enable = true;
	process {
	    user = "nobody";
	}
	EOF

	rcd_start "userdrop"
	rcd_wait "userdrop"

	sleep 2

	atf_check -s exit:0 -o match:"nobody" \
	    jexec "userdrop" cat /var/tmp/whoami
}

rcdexec_user_drop_cleanup()
{
	rcd_cleanup
}

# ---------------------------------------------------------------
# Test: nostart keyword allows manual start but skips boot
# ---------------------------------------------------------------
atf_test_case nostart_manual cleanup
nostart_manual_head()
{
	atf_set descr "nostart service can be started manually via rcctl"
	atf_set require.user root
	atf_set timeout 60
}

nostart_manual_body()
{
	rcd_init

	rcd_mkjail "nostart"

	rcd_add_unit "nostart" "manual_svc.ucl" <<-EOF
	name = "manual_svc";
	type = "oneshot";
	command = "/usr/bin/touch";
	command_args = "/var/tmp/manual_ran";
	provides = ["manual_svc"];
	enable = true;
	keywords = ["nostart"];
	EOF

	rcd_start "nostart"
	rcd_wait "nostart"

	# Should NOT have run at boot (nostart skips it)
	atf_check -s exit:1 \
	    jexec "nostart" test -f /var/tmp/manual_ran

	# Status should show nostart
	atf_check -s exit:0 -o match:"manual_svc.*nostart" \
	    jexec "nostart" /sbin/rcctl status manual_svc

	# But manual start should work
	atf_check -s exit:0 \
	    jexec "nostart" /sbin/rcctl start manual_svc

	# Now the marker should exist
	atf_check -s exit:0 \
	    jexec "nostart" test -f /var/tmp/manual_ran
}

nostart_manual_cleanup()
{
	rcd_cleanup
}

# ---------------------------------------------------------------

atf_init_test_cases()
{
	atf_add_test_case boot_in_jail
	atf_add_test_case rcctl_status
	atf_add_test_case barriers_done
	atf_add_test_case oneshot_completes
	atf_add_test_case nojail_status
	atf_add_test_case stop_start
	atf_add_test_case legacy_wrap
	atf_add_test_case disabled_status
	atf_add_test_case safe_upgrade
	atf_add_test_case dep_ordering
	atf_add_test_case restart_on_failure
	atf_add_test_case precmd_abort
	atf_add_test_case enable_disable_persist
	atf_add_test_case rcdexec_user_drop
	atf_add_test_case nostart_manual
}
