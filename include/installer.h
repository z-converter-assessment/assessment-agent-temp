/**
 * @file installer.h
 * @brief Self-install / -uninstall / -image-prep subcommands (Linux).
 *
 * Replaces the deploy/install.sh + deploy/uninstall.sh + scripts/image-prep.sh
 * trio at the *invocation* surface. Operators ship the single ELF and drive
 * the full lifecycle through it:
 *
 *     sudo ./assessment-agent install              # register + start
 *     sudo ./assessment-agent install --image-prep # register, do NOT start
 *     sudo ./assessment-agent uninstall            # stop + remove
 *     sudo ./assessment-agent uninstall --purge    # also wipe state + user
 *     sudo ./assessment-agent prep-image           # clear machine-id, etc.
 *
 * Implementation: the sh scripts and systemd unit + agent.env.example are
 * embedded into the ELF as RT_RCDATA-equivalent sections via `ld -r -b binary`
 * (see Makefile). The subcommand stages them into a private /tmp directory
 * with deploy/ layout and execs /bin/sh against the entry point.
 *
 * Why sh-embedded instead of porting to C: install.sh already exercises
 * useradd / systemctl / install / chown via their natural sh interface, and
 * those are the canonical CLIs on every supported Linux. Reimplementing them
 * in C only re-wraps execvp+waitpid+errno — no leverage gained.
 */

#ifndef ASSESSMENT_AGENT_INSTALLER_H
#define ASSESSMENT_AGENT_INSTALLER_H

/* image_prep != 0 → registers service but does not start it (golden image). */
int installer_run_install(int image_prep);

/* purge != 0 → also wipes /etc/assessment-agent + /var/lib/agent-worker and
 * removes the assessment-agent user/group. Default keeps them so re-install
 * preserves credentials. */
int installer_run_uninstall(int purge);

int installer_run_prep_image(void);

#endif
