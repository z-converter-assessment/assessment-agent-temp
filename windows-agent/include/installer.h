#ifndef ASSESSMENT_AGENT_INSTALLER_H
#define ASSESSMENT_AGENT_INSTALLER_H

int installer_run_install(int image_prep);

int installer_run_uninstall(void);

int installer_run_prep_image(int run_sysprep);

#endif
