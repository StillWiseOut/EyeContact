#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#include <AE_General.r>
#endif

resource 'PiPL' (16000) {
	{
		Kind {
			AEEffect
		},
		Name {
			"EyeContact"
		},
		Category {
			"Maxine"
		},
#ifdef AE_OS_WIN
	#if defined(AE_PROC_INTELx64)
		CodeWin64X86 {"EffectMain"},
	#elif defined(AE_PROC_ARM64)
		CodeWinARM64 {"EffectMain"},
	#endif
#endif
		AE_PiPL_Version {
			2,
			0
		},
		AE_Effect_Spec_Version {
			PF_PLUG_IN_VERSION,
			PF_PLUG_IN_SUBVERS
		},
		/* Must equal PF_VERSION(1,0,0,PF_Stage_DEVELOP,1) set in GlobalSetup */
		AE_Effect_Version {
			524289
		},
		AE_Effect_Info_Flags {
			0
		},
		/* PF_OutFlag_DEEP_COLOR_AWARE */
		AE_Effect_Global_OutFlags {
			0x02000000
		},
		/* PF_OutFlag2_SUPPORTS_THREADED_RENDERING */
		AE_Effect_Global_OutFlags_2 {
			0x08000000
		},
		AE_Effect_Match_Name {
			"Maxine EyeContact"
		},
		AE_Reserved_Info {
			0
		},
		AE_Effect_Support_URL {
			"https://developer.nvidia.com/maxine"
		}
	}
};
