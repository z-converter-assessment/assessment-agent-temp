/**
 * @file installer.h
 * @brief Self-install / -uninstall / -image-prep subcommands.
 *
 * These replace the deploy/install.ps1 + deploy/env-setup.ps1 +
 * scripts/image-prep.ps1 trio. Operators ship `assessment-agent.exe` alone
 * and drive the full lifecycle through it:
 *
 *     PS> .\assessment-agent.exe install
 *     PS> .\assessment-agent.exe install --image-prep
 *     PS> .\assessment-agent.exe uninstall
 *     PS> .\assessment-agent.exe prep-image
 *     PS> .\assessment-agent.exe prep-image --sysprep
 *
 * agent.env.example is embedded as PE RT_RCDATA (see src/resources.rc) and
 * drives the canonical key list / prompt loop — no on-disk template needed.
 */

#ifndef ASSESSMENT_AGENT_INSTALLER_H
#define ASSESSMENT_AGENT_INSTALLER_H

/* Install subcommand.
 *   image_prep != 0 → register the service but do not start it (used before
 *                     sealing a golden VM image). */
int installer_run_install(int image_prep);

/* Uninstall subcommand — stop + delete service. Leaves agent.env / state
 * directories on disk so a re-install does not lose configuration. */
int installer_run_uninstall(void);

/* prep-image subcommand — regenerate HKLM MachineGuid so cloned VMs do not
 * collide on machine_id. run_sysprep != 0 also kicks Sysprep /generalize. */
int installer_run_prep_image(int run_sysprep);

#endif
