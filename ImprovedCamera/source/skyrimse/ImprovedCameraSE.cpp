/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

 // Precompiled Header
#include "stdafx.h"

#include "skyrimse/ImprovedCameraSE.h"

#include "cameras/Cameras.h"
#include "utils/PatternScan.h"
#include "utils/Log.h"
#include "utils/ICMath.h"
#include "skyrimse/Helper.h"


namespace ImprovedCamera {

	using namespace Address::Variable;

	ImprovedCameraSE::ImprovedCameraSE()
	{
		m_pluginConfig = DLLMain::Plugin::Get()->Config();
		SetupCameraData();
	}

	void ImprovedCameraSE::UpdateSwitchPOV()
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		auto camera = RE::PlayerCamera::GetSingleton();
		std::uint32_t cameraID = camera->currentState->id;

		switch (cameraID)
		{
			case RE::CameraStates::kFirstPerson:
			{
				// MapMenu detection - Do nothing for now.
				if (m_PreviousCameraID == RE::CameraStates::kTween) {}
				// Tween detection - Do nothing for now.
				if (m_CurrentCameraID == RE::CameraStates::kTween) {}
				// RaceSex Menu detection
				bool raceMenu = RE::UI::GetSingleton()->IsMenuOpen("RaceSex Menu");
				if (raceMenu) camera->worldFOV = *fDefaultWorldFOV;

				m_LastStateID = 0;
				m_IsFirstPerson = true;
				m_IsThirdPersonForced = false;
				break;
			}
			case RE::CameraStates::kThirdPerson:
			{
				auto thirdpersonState = (RE::ThirdPersonState*)camera->currentState.get();
				if (!thirdpersonState->IsInputEventHandlingEnabled() || player->IsInKillMove())
				{
					m_IsFirstPerson = true;
				}
				break;
			}
			// Following 3 only trigger if previous cameraID was RE::CameraStates::kFirstPerson.
			case RE::CameraStates::kMount:
			{
				auto horseCameraState = (RE::HorseCameraState*)camera->currentState.get();
				horseCameraState->targetZoomOffset = *fMinCurrentZoom;
				break;
			}
			case RE::CameraStates::kDragon:
			{
				auto dragonCameraState = (RE::ThirdPersonState*)camera->currentState.get();
				dragonCameraState->targetZoomOffset = *fMinCurrentZoom;
				break;
			}
			// Triggers when wood chopping or mining.
			case RE::CameraStates::kFurniture:
			{
				break;
			}
		}
		// Fixes Main Menu crash if you decide to click on ResetState.
		//    Note: Heavy load order will come inside this function when the game isn't fully loaded!
		auto thirdperson3D = RE::PlayerCharacter::GetSingleton()->Get3D(0);
		if (!thirdperson3D)
			return;

		auto thirdpersonNode = thirdperson3D->AsNode();

		if (cameraID == RE::CameraStates::kFirstPerson)
		{
			bool consoleMenu = RE::UI::GetSingleton()->IsMenuOpen("Console");

			if (!m_pluginConfig->General().bEnableBody || (consoleMenu && !m_pluginConfig->General().bEnableBodyConsole))
			{
				thirdpersonNode->GetFlags().set(RE::NiAVObject::Flag::kHidden);
				return;
			}
		}
		if (cameraID != RE::CameraStates::kFirstPerson)
			DisplayShadows(true);

		thirdpersonNode->GetFlags().reset(RE::NiAVObject::Flag::kHidden);
	}

	void ImprovedCameraSE::UpdateCamera(std::uint8_t currentID, std::uint8_t previousID)
	{
		m_CurrentCameraID = currentID;
		m_PreviousCameraID = previousID;
		// Stop processing on Tween
		if (m_CurrentCameraID == RE::CameraStates::kTween)
		{
			UpdateFirstPerson(); // Force update
			return;
		}
		// Stop processing on MapMenu
		bool mapMenu = RE::UI::GetSingleton()->IsMenuOpen("MapMenu");
		if (mapMenu) return;

		auto player = RE::PlayerCharacter::GetSingleton();
		auto playerCamera = RE::PlayerCamera::GetSingleton();

		for (Interface::ICamera* camera : m_Camera)
		{
			if (camera->OnUpdate(currentID, previousID))
			{
				// Forces the camera back to first person if exiting the dragon from first person
				if (camera->GetID() == RE::CameraStates::kThirdPerson && previousID == RE::CameraStates::kPCTransition
					&& m_CameraEventID == CameraEvent::kDragonTransition && m_IsFirstPerson)
				{
					playerCamera->ForceFirstPerson();
					return;
				}
				// Animation camera is harder to detect what is happening, just use previous CameraEventID
				if (camera->GetID() != RE::CameraStates::kAnimated)
					m_CameraEventID = camera->GetEventID();
#if _DEBUG
				LOG_INFO("UpdateCamera:\t\tCamera: {} ({}) - StateID: {} - CameraEventID: {}", camera->GetName().c_str(), camera->GetID(), camera->GetStateID(), m_CameraEventID);
#endif
			}
			if (camera->HasControl())
			{
				m_ICamera = camera;
			}
		}
		// m_ICamera should never be null
		if (!m_ICamera)
			return;

		UpdateNearDistance(playerCamera);
		UpdateFOV(playerCamera);

		// Check FirstPerson camera
		if (currentID == RE::CameraStates::kFirstPerson)
		{
			m_IsFirstPerson = true;
			m_IsFakeCamera = false;
			if (m_ICamera->GetStateID() == CameraFirstPerson::State::kFishingIdle)
				m_LastStateID = CameraFirstPerson::State::kFishingIdle;

			// We force cartride to use the alternative camera.
			if (m_ICamera->GetStateID() >= CameraFirstPerson::State::kCartRideEnter)
				m_IsFakeCamera = true;
		}
		// Fixes issue with player following Vanity Camera
		else if (currentID == RE::CameraStates::kAutoVanity)
		{
			m_IsFirstPerson = false;
			m_IsFakeCamera = false;
		}
		// Fixes flyback issue with Sawmill failing to load a log
		//    Note: Game engine bug, you can get stuck and just need to click the logs until it frees itself.
		else if (currentID == RE::CameraStates::kFurniture && m_ICamera->GetID() == RE::CameraStates::kFirstPerson)
		{
			m_IsFakeCamera = true;
		}
		// Fixes Scripted/Animation to force back into first person once finished and also Horse/Dragon if were in fake first person.
		else if (m_ICamera->GetID() == RE::CameraStates::kThirdPerson &&
			(m_ICamera->GetStateID() >= CameraThirdPerson::State::kEnter && m_ICamera->GetStateID() <= CameraThirdPerson::State::kWeaponDrawnIdle) &&
			(m_LastStateID == CameraThirdPerson::State::kScriptedIdle || m_LastStateID == CameraThirdPerson::State::kAnimationIdle
				|| m_PreviousCameraID == RE::CameraStates::kMount || m_PreviousCameraID == RE::CameraStates::kDragon) && m_IsThirdPersonForced)
		{
			if (!Helper::IsInteracting(player))
			{
				m_LastStateID = 0;
				m_IsThirdPersonForced = false;
				playerCamera->ForceFirstPerson();
				return;
			}
		}
		// Check all other cameras
		else if (currentID > RE::CameraStates::kFirstPerson && m_ICamera->GetID() > RE::CameraStates::kFirstPerson)
		{
			m_IsFakeCamera = true;

			// Fixes issue where camera does not reset m_IsFirstPerson.
			if (m_ICamera->GetID() == RE::CameraStates::kThirdPerson && m_ICamera->GetStateID() == CameraThirdPerson::State::kIdle &&
				m_LastStateID && m_LastStateID != CameraThirdPerson::State::kScriptedIdle && m_LastStateID != CameraThirdPerson::State::kAnimationIdle && m_IsFirstPerson)
			{
				m_LastStateID = 0;
				m_IsFirstPerson = false;
			}
			// Ignore DeathCinematic for first person bows
			if (m_ICamera->GetID() == RE::CameraStates::kVATS && previousID == RE::CameraStates::kFirstPerson && (Helper::IsAiming(player) || Helper::IsAiming(player, true)))
				m_IsFirstPerson = false;

			// Ignore unknown state, happens due to camera's switching and doesn't know the real event was disabled.
			if (m_ICamera->GetID() == RE::CameraStates::kPCTransition && m_ICamera->GetStateID() == CameraTransition::State::kPlaying
				|| currentID == RE::CameraStates::kAnimated && m_CameraEventID == CameraEvent::kScripted)
			{
				m_IsFirstPerson = false;
			}
			// Ignore normal Third Person
			if (m_ICamera->GetID() == RE::CameraStates::kThirdPerson &&
				m_ICamera->GetStateID() > CameraThirdPerson::State::kExit && m_ICamera->GetStateID() < CameraThirdPerson::State::kCraftingEnter && !m_IsThirdPersonForced)
			{
				m_IsFirstPerson = false;
				m_IsFakeCamera = false;
				ReleaseAPIs();
			}
			// Ignore interacting with furniture
			if (m_ICamera->GetID() == RE::CameraStates::kFurniture && m_ICamera->GetStateID() == CameraFurniture::State::kIdle)
				m_IsFakeCamera = false;

			// Control the mouse wheel zoom in/out for Fake First Person
			if (m_ICamera->GetID() == RE::CameraStates::kThirdPerson && m_ICamera->GetStateID() >= CameraThirdPerson::State::kScriptedEnter
				|| m_ICamera->GetID() == RE::CameraStates::kMount && m_ICamera->GetStateID() >= CameraHorse::State::kMounted && m_ICamera->GetStateID() <= CameraHorse::State::kWeaponDrawnIdle
				|| m_ICamera->GetID() == RE::CameraStates::kDragon && m_ICamera->GetStateID() >= CameraDragon::State::kMounted && m_ICamera->GetStateID() <= CameraDragon::State::kRiding)
			{
				if (m_IsFirstPerson)
				{
					auto thirdpersonState = (RE::ThirdPersonState*)playerCamera->currentState.get();
					float zoom = *fMinCurrentZoom + m_pluginConfig->Fixes().fSwitchPOVDetectDistance;
					m_LastStateID = m_ICamera->GetStateID();
					m_IsFakeCamera = false;
					m_IsThirdPersonForced = false;

					if (thirdpersonState->targetZoomOffset <= zoom)
					{
						// Need to set the following to block entering first person.
						thirdpersonState->currentZoomOffset = zoom;
						thirdpersonState->targetZoomOffset = -0.2; // Default savedZoomOffset: -0.2
						m_IsFirstPerson = true;
						m_IsFakeCamera = true;
						m_IsThirdPersonForced = true;

						if (m_SmoothCamAPI)
						{
							auto smoothCamResult = m_SmoothCamAPI->RequestCameraControl(m_ICHandle);
							if (smoothCamResult == SmoothCamAPI::APIResult::OK) {} // Just to stop compiler moaning
						}
						if (m_TDMAPI)
						{
							auto tdmHeadTrackingResult = m_TDMAPI->RequestDisableHeadtracking(m_ICHandle);
							if (tdmHeadTrackingResult == TDM_API::APIResult::OK) {} // Just to stop compiler moaning

							if (!m_TDMSnapshot)
							{
								m_TDMSnapshot = true;
								m_directionalMovementSheathedOriginal = *m_directionalMovementSheathed;
								m_directionalMovementDrawnOriginal = *m_directionalMovementDrawn;
							}
							*m_directionalMovementSheathed = TDM_API::DirectionalMovementMode::kDisabled;
							*m_directionalMovementDrawn = TDM_API::DirectionalMovementMode::kDisabled;
						}
					}
					else
					{
						ReleaseAPIs();
					}
				}
			}
			// Reset m_LastStateID upon player death
			if (m_ICamera->GetID() == RE::CameraStates::kBleedout && m_ICamera->GetStateID() == CameraRagdoll::State::kPlayingDeath && m_LastStateID)
				m_LastStateID = 0;

			// Don't force first person mode
			if (!m_IsFirstPerson)
				m_IsFakeCamera = false;

			// Do we want to process this event?
			if (!*m_ICamera->GetData().EventActive)
			{
				m_IsFirstPerson = false;
				m_IsFakeCamera = false;
			}
		}
		// Good to go start translating the camera
		if (m_CurrentCameraID == RE::CameraStates::kFirstPerson || m_IsFakeCamera)
			TranslateCamera();
	}

	void ImprovedCameraSE::UpdateFirstPerson()
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		auto firstpersonNode = player->Get3D(1)->AsNode();
		auto thirdpersonNode = player->Get3D(0)->AsNode();
		auto camera = RE::PlayerCamera::GetSingleton();
		auto cameraID = camera->currentState->id;
		// Don't process on AutoVanity
		if (cameraID == RE::CameraStates::kAutoVanity) return;
		// Don't process on TFC
		static bool resetFOV;
		if (cameraID == RE::CameraStates::kFree)
		{
			if (!resetFOV)
			{
				resetFOV = true;
				m_IsFakeCamera = false;
				UpdateSkeleton(true);
				camera->worldFOV = *fDefaultWorldFOV;
			}
			return;
		}
		resetFOV = false;

		m_thirdpersonLocalTranslate = thirdpersonNode->local.translate;

		if (m_iRagdollFrame)
		{
			m_iRagdollFrame++;

			if (m_iRagdollFrame > 6)
				m_iRagdollFrame = 0;
		}

		// Reset head states when m_IsFakeCamera is switched off.
		auto thirdpersonState = (RE::ThirdPersonState*)RE::PlayerCamera::GetSingleton()->currentState.get();
		if (cameraID != RE::CameraStates::kFirstPerson && thirdpersonState->targetZoomOffset > *fMinCurrentZoom)
		{
			if (!m_ICamera) return;

			bool isWerewolf = m_ICamera->GetID() == RE::CameraStates::kThirdPerson && m_ICamera->GetStateID() == CameraThirdPerson::State::kWerewolfIdle;
			auto headNode = Helper::GetHeadNode(thirdpersonNode);

			if (isWerewolf)
				headNode = thirdpersonNode->GetObjectByName("NPC Neck [Neck]")->AsNode();

			if (headNode && headNode->local.scale < 0.002f)
			{
				UpdateSkeleton(true);
				Helper::UpdateNode(thirdpersonNode);
			}
		}

		if (cameraID == RE::CameraStates::kFirstPerson || m_CurrentCameraID == RE::CameraStates::kTween && m_PreviousCameraID == RE::CameraStates::kFirstPerson
			|| m_IsFakeCamera)
		{
			// Update player scale
			UpdateFirstPersonScale();

			// Update arms
			auto leftarmNode = firstpersonNode->GetObjectByName("NPC L UpperArm [LUar]")->AsNode();
			auto rightarmNode = firstpersonNode->GetObjectByName("NPC R UpperArm [RUar]")->AsNode();
			float armScale = UseThirdPersonArms() ? 0.001f : 1.0f;

			leftarmNode->local.scale = armScale;
			rightarmNode->local.scale = armScale;

			if (UseThirdPersonLeftArm()) leftarmNode->local.scale = 0.001f;
			if (UseThirdPersonRightArm()) rightarmNode->local.scale = 0.001f;

			TranslateThirdPerson();
			// Height offset
			thirdpersonNode->local.translate.z += m_pluginConfig->General().fBodyHeightOffset;

			UpdateSkeleton(false);
			Helper::UpdateNode(thirdpersonNode);
			UpdateSkeleton(true);

			DisplayShadows(m_pluginConfig->General().bEnableShadows);

			float headRot = 0.0f;
			if (GetHeadRotation(&headRot) || m_IsFakeCamera)
			{
				TranslateFirstPerson();
				// Height offset
				firstpersonNode->local.translate.z += m_pluginConfig->General().fBodyHeightOffset;
			}

			// Fixes weapons drawing when sitting/sleeping
			if (m_ICamera && m_ICamera->GetID() == RE::CameraStates::kFirstPerson && m_ICamera->GetStateID() >= CameraFirstPerson::State::kSittingEnter
				&& firstpersonNode->local.scale > 0.002f)
			{
				firstpersonNode->local.scale = 0.001f;
				Helper::UpdateNode(firstpersonNode, RE::NiUpdateData::Flag::kDisableCollision);
				Helper::UpdateNode(thirdpersonNode, RE::NiUpdateData::Flag::kDisableCollision);
				return;
			}
			Helper::UpdateNode(firstpersonNode);
		}
		// Fix crosshair issue in Third Person due to firstpersonNode shrunk.
		if (firstpersonNode->local.scale == 0.001f)
			UpdateFirstPersonScale();
	}

	bool ImprovedCameraSE::SmoothAnimationTransitions()
	{
		const auto camera = RE::PlayerCamera::GetSingleton();

		if (!camera->IsInFirstPerson())
		{
			*fControllerBufferDepth = m_pluginConfig->Fixes().fControllerBufferDepth3rd;
			return true;
		}
		if (!m_pluginConfig->Fixes().bSmoothAnimationTransitions)
			return false;

		auto controlMap = RE::ControlMap::GetSingleton();
		if (!controlMap->IsMovementControlsEnabled())
			return false;

		*fControllerBufferDepth = m_pluginConfig->Fixes().fControllerBufferDepth1st;
		return true;
	}

	bool ImprovedCameraSE::UpdateHeadTracking()
	{
		auto camera = RE::PlayerCamera::GetSingleton();
		auto player = RE::PlayerCharacter::GetSingleton();

		bool onMount = Helper::IsOnMount(player);
		bool mounted = onMount && Helper::IsInteracting(player);

		// Ignore Furniture
		if (m_CameraEventID == CameraEvent::kFurniture && (camera->IsInFirstPerson() || m_IsFakeCamera))
			return false;
		// Ignore Crafting
		if (m_CameraEventID == CameraEvent::kCrafting && (camera->IsInFirstPerson() || m_IsFakeCamera))
			return false;
		// Ignore Scripted
		if (m_CameraEventID == CameraEvent::kScripted && (camera->IsInFirstPerson() || m_IsFakeCamera))
			return false;

		/*// Only here for fun
		auto player = RE::PlayerCharacter::GetSingleton();

		if (camera->IsInFirstPerson() && player->AsActorState()->IsWeaponDrawn())
			return false;
		*/
		return (!onMount || mounted);
	}

	void ImprovedCameraSE::ModelReferenceEffectFix1(void* arg1, RE::Actor* actor)
	{
		using namespace Address::Function;

		auto player = RE::PlayerCharacter::GetSingleton();
		auto camera = RE::PlayerCamera::GetSingleton();

		if (actor != player || !camera->IsInFirstPerson())
		{
			ModelReferenceEffect_Attach(arg1);
			return;
		}
		// Fix model fx when fighting with third person arms
		if (player->AsActorState()->IsWeaponDrawn() && !UseThirdPersonArms())
		{
			ModelReferenceEffect_Attach(arg1);
		}
		else
		{
			if (!m_pluginConfig->Fixes().bQuickLightLighting)
			{
				stl::enumeration<RE::PlayerCharacter::FlagBDB, std::uint8_t> saveState = player->GetPlayerRuntimeData().unkBDB;
				player->GetPlayerRuntimeData().unkBDB.set(RE::PlayerCharacter::FlagBDB::kIsInThirdPersonMode);
				ModelReferenceEffect_Attach(arg1);
				player->GetPlayerRuntimeData().unkBDB = saveState;
			}
			else
				ModelReferenceEffect_Attach(arg1);
		}
	}

	bool ImprovedCameraSE::ModelReferenceEffectFix2(void* arg1, RE::Actor* actor)
	{
		using namespace Address::Function;

		auto player = RE::PlayerCharacter::GetSingleton();
		auto camera = RE::PlayerCamera::GetSingleton();

		if (actor != player || !camera->IsInFirstPerson())
		{
			return ModelReferenceEffect_Sub_14057BCC0(arg1);
		}

		bool rtn = false;
		// Fix model fx when fighting with third person arms
		if (player->AsActorState()->IsWeaponDrawn() && !UseThirdPersonArms())
		{
			rtn = ModelReferenceEffect_Sub_14057BCC0(arg1);
		}
		else
		{
			if (!m_pluginConfig->Fixes().bQuickLightLighting)
			{
				stl::enumeration<RE::PlayerCharacter::FlagBDB, std::uint8_t> saveState = player->GetPlayerRuntimeData().unkBDB;
				player->GetPlayerRuntimeData().unkBDB.set(RE::PlayerCharacter::FlagBDB::kIsInThirdPersonMode);
				rtn = ModelReferenceEffect_Sub_14057BCC0(arg1);
				player->GetPlayerRuntimeData().unkBDB = saveState;
			}
			else
				rtn = ModelReferenceEffect_Sub_14057BCC0(arg1);
		}
		return rtn;
	}

	bool ImprovedCameraSE::ShaderReferenceEffectFix1(void* arg1, RE::Actor* actor)
	{
		using namespace Address::Function;

		auto player = RE::PlayerCharacter::GetSingleton();
		auto camera = RE::PlayerCamera::GetSingleton();

		if (actor != player || !camera->IsInFirstPerson())
		{
			return ShaderReferenceEffect_Sub_140584680(arg1);
		}

		bool rtn = false;
		// Only shade the arms if fighting and not using third person arms
		if (player->AsActorState()->IsWeaponDrawn() && !UseThirdPersonArms())
		{
			rtn = ShaderReferenceEffect_Sub_140584680(arg1);
		}
		else
		{
			stl::enumeration<RE::PlayerCharacter::FlagBDB, std::uint8_t> saveState = player->GetPlayerRuntimeData().unkBDB;
			player->GetPlayerRuntimeData().unkBDB.set(RE::PlayerCharacter::FlagBDB::kIsInThirdPersonMode);
			rtn = ShaderReferenceEffect_Sub_140584680(arg1);
			player->GetPlayerRuntimeData().unkBDB = saveState;
		}
		return rtn;
	}

	void ImprovedCameraSE::ShaderReferenceEffectFix2(void* arg1, RE::Actor* actor)
	{
		typedef void(*FuncType_Void_Void)(void*);
		FuncType_Void_Void ShaderReferenceEffect2 = (FuncType_Void_Void)Helper::GetVTableAddress(arg1, 0x1D8);

		auto player = RE::PlayerCharacter::GetSingleton();
		auto camera = RE::PlayerCamera::GetSingleton();

		if (actor != player || !camera->IsInFirstPerson())
		{
			ShaderReferenceEffect2(arg1);
			return;
		}
		// Only shade the arms if fighting and not using third person arms
		if (player->AsActorState()->IsWeaponDrawn() && !UseThirdPersonArms())
		{
			ShaderReferenceEffect2(arg1);
		}
		else
		{
			stl::enumeration<RE::PlayerCharacter::FlagBDB, std::uint8_t> saveState = player->GetPlayerRuntimeData().unkBDB;
			player->GetPlayerRuntimeData().unkBDB.set(RE::PlayerCharacter::FlagBDB::kIsInThirdPersonMode);
			ShaderReferenceEffect2(arg1);
			player->GetPlayerRuntimeData().unkBDB = saveState;
		}
	}

	void ImprovedCameraSE::ResetPlayerNodes()
	{
		using namespace Address::Function;

		auto player = RE::PlayerCharacter::GetSingleton();

		if (!player)
			return;

		const auto magicEffectNodes = player->GetActorRuntimeData().magicCasters;
		for (auto p = magicEffectNodes; p < magicEffectNodes + 4; p++)
		{
			if (p && *p)
				ResetNodes(*p);
		}
	}

	void ImprovedCameraSE::ForceFirstPerson()
	{
#if _DEBUG
		LOG_INFO("ForceFirstPerson:\tPrevious Camera: {} - CameraEventID: {}", m_ICamera->GetID(), m_CameraEventID);
#endif
		m_IsFirstPerson = true;
		m_IsFakeCamera = false;

		// Fixes mods/scripts attempting to break the camera.
		//    9/10 animations do not work in first person.
		auto player = RE::PlayerCharacter::GetSingleton();
		auto camera = RE::PlayerCamera::GetSingleton();
		// Bandaid fix for Triumvirate added
		if (Helper::IsInteracting(player) || strcmp(player->GetRace()->GetName(), "Druid Deer") == 0)
		{
			camera->ForceThirdPerson();
		}
	}

	void ImprovedCameraSE::ForceThirdPerson()
	{
#if _DEBUG
		LOG_INFO("ForceThirdPerson:\tPrevious Camera: {} - CameraEventID: {}", m_ICamera->GetID(), m_CameraEventID);
#endif
		m_IsFirstPerson = false;
		m_IsFakeCamera = false;
		m_IsThirdPersonForced = false;

		if (m_ICamera && m_ICamera->GetID() == RE::CameraState::kFirstPerson)
		{
			if (m_LastStateID != CameraFirstPerson::State::kFishingIdle)
			{
				m_IsFirstPerson = true;
				m_IsFakeCamera = true;
				m_IsThirdPersonForced = true;
			}
			m_LastStateID = 0;
		}
	}

	void ImprovedCameraSE::TogglePOV()
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		// Bandaid fix for Triumvirate added
		if (!m_ICamera || strcmp(player->GetRace()->GetName(), "Druid Deer") == 0)
			return;

		auto thirdpersonState = (RE::ThirdPersonState*)RE::PlayerCamera::GetSingleton()->currentState.get();
		float minCurrentZoom = *fMinCurrentZoom;

		if (m_ICamera->GetID() == RE::CameraStates::kThirdPerson && m_ICamera->GetStateID() >= CameraThirdPerson::State::kScriptedEnter)
		{
			if (thirdpersonState->targetZoomOffset == minCurrentZoom)
				thirdpersonState->targetZoomOffset = -0.125f;
			else
			{
				m_IsFirstPerson = true;
				thirdpersonState->targetZoomOffset = minCurrentZoom;
			}
		}
		else if (m_ICamera->GetID() == RE::CameraStates::kMount && m_ICamera->GetStateID() >= CameraHorse::State::kRiding && m_ICamera->GetStateID() <= CameraHorse::State::kWeaponDrawnIdle)
		{
			if (thirdpersonState->targetZoomOffset == minCurrentZoom)
				thirdpersonState->targetZoomOffset = -0.125f;
			else
			{
				m_IsFirstPerson = true;
				thirdpersonState->targetZoomOffset = minCurrentZoom;
			}
		}
		else if (m_ICamera->GetID() == RE::CameraStates::kDragon && m_ICamera->GetStateID() == CameraDragon::State::kRiding)
		{
			if (thirdpersonState->targetZoomOffset == minCurrentZoom)
				thirdpersonState->targetZoomOffset = -0.125f;
			else
			{
				m_IsFirstPerson = true;
				thirdpersonState->targetZoomOffset = minCurrentZoom;
			}
		}
	}

	void ImprovedCameraSE::ResetState(bool forced)
	{
		auto ui = RE::UI::GetSingleton();
		bool mainMenu = ui->IsMenuOpen("Main Menu");
		bool loadingMenu = ui->IsMenuOpen("Loading Menu");
		auto player = RE::PlayerCharacter::GetSingleton();

		if (!player)
			return;

		if (forced && !mainMenu && !loadingMenu && !Helper::IsBeastMode() && !player->IsOnMount())
		{
			auto camera = RE::PlayerCamera::GetSingleton();
			auto controlMap = RE::ControlMap::GetSingleton();
			auto playerControls = RE::PlayerControls::GetSingleton();
			auto firstperson3D = RE::PlayerCharacter::GetSingleton()->Get3D(1);
			auto thirdpersonState = (RE::ThirdPersonState*)camera->cameraStates[RE::CameraState::kThirdPerson].get();
			// Fixes interaction with crafting/furniture
			//    Certain objects are used from firstperson node, this should not be hidden.
			if (camera->IsInFirstPerson() && firstperson3D)
			{
				auto firstpersonNode = firstperson3D->AsNode();
				firstpersonNode->GetFlags().reset(RE::NiAVObject::Flag::kHidden);
			}
			// Fixes inputEventHandling, crafting usually disables this.
			thirdpersonState->inputEventHandlingEnabled = true;
			// Fixes common enabledControls issues.
			controlMap->enabledControls.set(RE::UserEvents::USER_EVENT_FLAG::kMovement, 
				RE::UserEvents::USER_EVENT_FLAG::kLooking,
				RE::UserEvents::USER_EVENT_FLAG::kPOVSwitch,
				RE::UserEvents::USER_EVENT_FLAG::kFighting);
			// Force to third person to correct any other issues.
			camera->ForceThirdPerson();
			// Fixes being stuck in Scripted state
			playerControls->data.povScriptMode = false;
			player->NotifyAnimationGraph("IdleForceDefaultState");

			m_IsThirdPersonForced = false;
		}

		m_IsFirstPerson = false;
		m_IsFakeCamera = false;

		UpdateSwitchPOV();
	}

	void ImprovedCameraSE::Ragdoll(RE::Actor* actor)
	{
		auto player = RE::PlayerCharacter::GetSingleton();

		if (actor == player)
		{
			auto thirdpersonNode = player->Get3D(0)->AsNode();
			thirdpersonNode->local.translate = m_thirdpersonLocalTranslate;
			Helper::UpdateNode(thirdpersonNode);
			m_iRagdollFrame = 1;
		}
	}

	bool ImprovedCameraSE::Ragdoll_IsTaskPoolRequired(RE::Actor* actor)
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		return (actor == player && m_iRagdollFrame < 2);
	}

	void ImprovedCameraSE::Ragdoll_UpdateObjectUpwards(RE::Actor* actor)
	{
		auto player = RE::PlayerCharacter::GetSingleton();

		if (actor == player)
		{
			if (m_iRagdollFrame < 2)
			{
				auto thirdpersonNode = player->Get3D(0)->AsNode();
				thirdpersonNode->local.translate = m_thirdpersonLocalTranslate;
				Helper::UpdateNode(thirdpersonNode);
			}
			m_iRagdollFrame = 0;
		}
	}

	void ImprovedCameraSE::SetElderScrollReading(bool reading)
	{
		m_ElderScrollReading = reading;
	}

	void ImprovedCameraSE::RequestAPIs()
	{
		m_ICHandle = SKSE::GetPluginHandle();

		if (!SmoothCamAPI::RegisterInterfaceLoaderCallback(SKSE::GetMessagingInterface(),
			[](void* interfaceInstance, SmoothCamAPI::InterfaceVersion interfaceVersion)
			{
				if (interfaceVersion == SmoothCamAPI::InterfaceVersion::V3)
					m_SmoothCamAPI = reinterpret_cast<SmoothCamAPI::IVSmoothCam3*>(interfaceInstance);
			}))	{}

		if (!SmoothCamAPI::RequestInterface(SKSE::GetMessagingInterface(), SmoothCamAPI::InterfaceVersion::V3)) {}

		m_TDMAPI = reinterpret_cast<TDM_API::IVTDM2*>(TDM_API::RequestPluginAPI(TDM_API::InterfaceVersion::V2));
		if (m_TDMAPI)
		{
			auto directionalMovementSheathed = Utils::FindPattern("TrueDirectionalMovement.dll", "02 00 00 00 02 00 00 00 02 00 00 00");
			auto directionalMovementDrawn = directionalMovementSheathed + 0x4;
			m_directionalMovementSheathed = (std::uint32_t*)directionalMovementSheathed;
			m_directionalMovementDrawn = (std::uint32_t*)directionalMovementDrawn;
		}
	}
	////////////////////////////////////////////////////////////////
	bool ImprovedCameraSE::UseThirdPersonArms()
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		auto playerState = player->AsActorState();

		if (m_ElderScrollReading)
			return false;

		if (m_CurrentCameraID == RE::CameraState::kFirstPerson && playerState->IsSprinting() && !playerState->IsWeaponDrawn()
			&& !Helper::IsBeastMode() && !m_pluginConfig->General().bEnableThirdPersonArms && m_pluginConfig->Fixes().bFirstPersonOverhaul)
			return false;

		if ((m_CurrentCameraID != RE::CameraState::kFirstPerson && !(m_CurrentCameraID == RE::CameraStates::kTween && m_PreviousCameraID == RE::CameraStates::kFirstPerson))
			|| Helper::IsOnMount(player) || (!playerState->IsWeaponDrawn() && !Helper::IsTorchOut(player)))
			return true;

		if (Helper::IsSitting(player) && Helper::IsTorchOut(player)) return true;
		if (Helper::IsAiming(player)) return m_pluginConfig->General().bEnableThirdPersonBowAim;
		if (Helper::IsRangedWeaponEquipped(player)) return m_pluginConfig->General().bEnableThirdPersonBow;
		if (Helper::IsAiming(player, true)) return m_pluginConfig->General().bEnableThirdPersonCrossbowAim;
		if (Helper::IsRangedWeaponEquipped(player, true)) return m_pluginConfig->General().bEnableThirdPersonCrossbow;

		return m_pluginConfig->General().bEnableThirdPersonArms;
	}

	bool ImprovedCameraSE::UseThirdPersonLeftArm()
	{
		using namespace Address::Function;

		auto player = RE::PlayerCharacter::GetSingleton();
		auto playerState = player->AsActorState();

		// CFPAO fix
		if (playerState->IsSprinting() && !playerState->IsWeaponDrawn() && !Helper::IsBeastMode() && m_pluginConfig->Fixes().bFirstPersonOverhaul)
			return false;

		// Fists fix
		if (GetEquippedItemTypeID(player) == RE::EQUIPPED_ITEMTYPE_ID::kFist && GetEquippedItemTypeID(player, true) == RE::EQUIPPED_ITEMTYPE_ID::kFist)
			return false;

		if (GetEquippedItemTypeID(player) == RE::EQUIPPED_ITEMTYPE_ID::kShield)
		{
			// Shield fix
			if (m_pluginConfig->General().bEnableThirdPersonShield && !player->IsBlocking())
				return true;

			// Shield Block fix
			if (m_pluginConfig->General().bEnableThirdPersonShieldBlock && player->IsBlocking())
				return true;
		}

		// One Handed weapon fix
		if (Helper::IsRighthandWeaponEquipped(player) && !player->IsBlocking() && !m_pluginConfig->Fixes().bFirstPersonOverhaul)
			return true;

		return false;
	}

	bool ImprovedCameraSE::UseThirdPersonRightArm()
	{
		using namespace Address::Function;

		auto player = RE::PlayerCharacter::GetSingleton();
		auto playerState = player->AsActorState();

		// CFPAO fix
		if (playerState->IsSprinting() && !playerState->IsWeaponDrawn() && !Helper::IsBeastMode() && m_pluginConfig->Fixes().bFirstPersonOverhaul)
			return false;

		// Torch fix
		if (Helper::IsTorchOut(player) && !player->AsActorState()->IsWeaponDrawn())
			return true;

		// Bow fix
		if (GetEquippedItemTypeID(player) == RE::EQUIPPED_ITEMTYPE_ID::kBow && !Helper::IsAiming(player) && !m_pluginConfig->Fixes().bArcheryGameplayOverhaul)
			return true;

		return false;
	}

	void ImprovedCameraSE::UpdateFirstPersonScale()
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		auto firstpersonNode = player->Get3D(1)->AsNode();
		auto thirdpersonNode = player->Get3D(0)->AsNode();

		if (m_pluginConfig->General().bAdjustPlayerScale)
		{
			if (firstpersonNode->local.scale != thirdpersonNode->local.scale)
				firstpersonNode->local.scale = thirdpersonNode->local.scale;
		}
		else
			firstpersonNode->local.scale = 1.0f;
	}

	void ImprovedCameraSE::UpdateSkeleton(bool show)
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		auto thirdpersonNode = player->Get3D(0)->AsNode();

		static std::vector<std::unique_ptr<NodeOverride>> overrides;

		// Don't clear if Tween is shown
		if (!m_TweenShown)
			overrides.clear(); // Reset all previous overrides

		m_TweenShown = false;

		// Make sure ICamera is ready.
		if (!m_ICamera)
			return;

		bool isWerewolf = m_ICamera->GetID() == RE::CameraStates::kThirdPerson && m_ICamera->GetStateID() == CameraThirdPerson::State::kWerewolfIdle;
		bool isVampireLord = m_ICamera->GetID() == RE::CameraStates::kThirdPerson && m_ICamera->GetStateID() == CameraThirdPerson::State::kVampireLordIdle;
		bool isScripted = m_ICamera->GetID() == RE::CameraStates::kThirdPerson && m_ICamera->GetStateID() == CameraThirdPerson::State::kScriptedIdle;

		// Hiding of the head in first/third person.
		if (!show)
		{
			// Tween comes in here for 1 frame, need to account for this so it doesn't screw up what has been done to the model(s).
			if (m_IsFakeCamera || m_CurrentCameraID == RE::CameraStates::kTween && m_PreviousCameraID == RE::CameraStates::kFirstPerson)
				m_TweenShown = true;

			auto headNode = Helper::GetHeadNode(thirdpersonNode);

			if (headNode && !Helper::CannotMoveAndLook())
			{
				bool hideHead = !m_pluginConfig->General().bEnableHead && (m_CurrentCameraID == RE::CameraStates::kFirstPerson || m_CurrentCameraID == RE::CameraStates::kTween && m_PreviousCameraID == RE::CameraStates::kFirstPerson) && m_ICamera->GetStateID() != CameraFirstPerson::State::kWeaponDrawnIdle
					|| !m_pluginConfig->General().bEnableHeadCombat && (m_CurrentCameraID == RE::CameraStates::kFirstPerson || m_CurrentCameraID == RE::CameraStates::kTween && m_PreviousCameraID == RE::CameraStates::kFirstPerson) && m_ICamera->GetStateID() == CameraFirstPerson::State::kWeaponDrawnIdle
					|| !m_pluginConfig->General().bEnableHeadHorse && m_IsFakeCamera && (m_CurrentCameraID == RE::CameraStates::kMount || m_CurrentCameraID == RE::CameraStates::kTween && m_PreviousCameraID == RE::CameraStates::kMount)
					|| !m_pluginConfig->General().bEnableHeadDragon && m_IsFakeCamera && (m_CurrentCameraID == RE::CameraStates::kDragon || m_CurrentCameraID == RE::CameraStates::kTween && m_PreviousCameraID == RE::CameraStates::kDragon)
					|| !m_pluginConfig->General().bEnableHeadScripted && m_IsFakeCamera && isScripted
					|| !m_pluginConfig->General().bEnableHeadVampireLord && m_IsFakeCamera && isVampireLord
					|| !m_pluginConfig->General().bEnableHeadWerewolf && m_IsFakeCamera && isWerewolf;

				if (hideHead && isWerewolf)
				{
					headNode = thirdpersonNode->GetObjectByName("NPC Neck [Neck]")->AsNode();

					auto thirdpersonState = (RE::ThirdPersonState*)RE::PlayerCamera::GetSingleton()->currentState.get();

					if (thirdpersonState->targetZoomOffset == *fMinCurrentZoom)
						overrides.push_back(std::make_unique<NodeOverride>(headNode, 0.001f));
				}
				else if (hideHead && headNode->local.scale > 0.002f)
				{
					overrides.push_back(std::make_unique<NodeOverride>(headNode, 0.001f));
				}
			}
		}
		// Hiding of the arms/weaponary for first person.
		if (!show)
		{
			if (m_CurrentCameraID != RE::CameraStates::kFirstPerson && !(m_CurrentCameraID == RE::CameraStates::kTween && m_PreviousCameraID == RE::CameraStates::kFirstPerson))
				return;

			if (isWerewolf || isVampireLord || isScripted)
				return;

			// Need this chunky block to check the calling Objects validity as calling AsNode on top will cause a crash if non-existent.
			auto leftArmObject = thirdpersonNode->GetObjectByName("NPC L UpperArm [LUar]");
			if (!leftArmObject) return;
			auto rightArmObject = thirdpersonNode->GetObjectByName("NPC R UpperArm [RUar]");
			if (!rightArmObject) return;
			auto weaponBackObject = thirdpersonNode->GetObjectByName("WeaponBack");
			if (!weaponBackObject) return;
			auto weaponBowObject = thirdpersonNode->GetObjectByName("WeaponBow");
			if (!weaponBowObject) return;
			auto quiverObject = thirdpersonNode->GetObjectByName("QUIVER");
			if (!quiverObject) return;

			auto leftArmNode = leftArmObject->AsNode();
			auto rightArmNode = rightArmObject->AsNode();
			auto weaponBackNode = weaponBackObject->AsNode();
			auto weaponBowNode = weaponBowObject->AsNode();
			auto quiverNode = quiverObject->AsNode();

			if (!UseThirdPersonArms())
			{
				if (!UseThirdPersonLeftArm() && leftArmNode->local.scale > 0.002f)
				{
					overrides.push_back(std::make_unique<NodeOverride>(leftArmNode, 0.001f));
				}
				if (!UseThirdPersonRightArm() && rightArmNode->local.scale > 0.002f)
				{
					overrides.push_back(std::make_unique<NodeOverride>(rightArmNode, 0.001f));
				}
			}
			if (m_pluginConfig->Hide().b2HWeapon && weaponBackNode->local.scale > 0.002f)
			{
				overrides.push_back(std::make_unique<NodeOverride>(weaponBackNode, 0.001f));
			}
			if (m_pluginConfig->Hide().bBow && weaponBowNode->local.scale > 0.002f)
			{
				overrides.push_back(std::make_unique<NodeOverride>(weaponBowNode, 0.001f));
			}
			if (m_pluginConfig->Hide().bQuiver && quiverNode->local.scale > 0.002f)
			{
				overrides.push_back(std::make_unique<NodeOverride>(quiverNode, 0.001f));
			}
		}
	}

	void ImprovedCameraSE::DisplayShadows(bool show)
	{
		auto thirdperson3D = RE::PlayerCharacter::GetSingleton()->Get3D(0);
		if (!thirdperson3D)
			return;

		RE::BSVisit::TraverseScenegraphGeometries(thirdperson3D, [&](RE::BSGeometry* a_geometry)->RE::BSVisit::BSVisitControl
			{
				const auto effect = a_geometry->properties[RE::BSGeometry::States::kEffect];
				const auto lightingShader = netimmerse_cast<RE::BSLightingShaderProperty*>(effect.get());

				if (lightingShader)
					lightingShader->SetFlags(RE::BSShaderProperty::EShaderPropertyFlag8::kCastShadows, show);

				return RE::BSVisit::BSVisitControl::kContinue;
			});
	}
	void ImprovedCameraSE::UpdateNearDistance(RE::PlayerCamera* camera)
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		auto firstpersonState = (RE::FirstPersonState*)camera->currentState.get();
		auto cameraNode = camera->cameraRoot.get()->AsNode();
		auto cameraNI = (RE::NiCamera*)((cameraNode->children.size() == 0) ? nullptr : cameraNode->children[0].get());
		if (!cameraNI) return;

		float nearDistance = *fNearDistance; // 15.0f
		float maxPitch = m_pluginConfig->NearDistance().fPitchThreshold;

		if (m_pluginConfig->NearDistance().bEnableOverride && *m_ICamera->GetData().EventActive)
		{
			bool dialogueMenu = RE::UI::GetSingleton()->IsMenuOpen("Dialogue Menu");

			if (m_ICamera->GetID() == RE::CameraStates::kFirstPerson)
			{
				if (Helper::IsSittingOrSleeping(player))
				{
					nearDistance = m_pluginConfig->NearDistance().fSitting;
				}
				else if (player->AsActorState()->IsWeaponDrawn())
				{
					nearDistance = *m_ICamera->GetData().NearDistance;
				}
				else if (firstpersonState->currentPitchOffset <= maxPitch)
				{
					nearDistance = *m_ICamera->GetData().NearDistance;
				}
				else
				{
					nearDistance = 15.0f;
				}
				// Fix for dismounting on the Intro cartride so the npc infront is fully seen. 
				if (m_ICamera->GetStateID() == CameraFirstPerson::State::kCartRideDismounting)
					nearDistance = 2.0f;
			}
			else if (!m_IsFakeCamera || dialogueMenu)
				nearDistance = m_pluginConfig->NearDistance().fThirdPerson;

			else
				nearDistance = *m_ICamera->GetData().NearDistance;
		}
		else
			nearDistance = 15.0f;

		// Reset fNearDistance and viewFrustum.fNear to default 15.0
		*fNearDistance = 15.0f;
		cameraNI->viewFrustum.fNear = *fNearDistance;

		if (cameraNI->viewFrustum.fNear != nearDistance)
		{
			*fNearDistance = nearDistance;
			cameraNI->viewFrustum.fNear = nearDistance;
		}
	}

	void ImprovedCameraSE::UpdateFOV(RE::PlayerCamera* camera)
	{
		float fov = *fDefaultWorldFOV; // 80.0f

		if (m_CurrentCameraID == RE::CameraStates::kFirstPerson)
		{
			float fovHands = m_pluginConfig->FOV().fFirstPersonHands; // 80.0f

			if (camera->firstPersonFOV != fovHands)
				camera->firstPersonFOV = fovHands;
		}
		// Don't update during Dialogue Menu
		bool dialogueMenu = RE::UI::GetSingleton()->IsMenuOpen("Dialogue Menu");
		if (!dialogueMenu)
		{
			if (m_pluginConfig->FOV().bEnableOverride && *m_ICamera->GetData().EventActive)
			{
				fov = *m_ICamera->GetData().FOV;

				if (camera->worldFOV != fov)
					camera->worldFOV = fov;
			}
		}
	}

	void ImprovedCameraSE::TranslateCamera()
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		auto camera = RE::PlayerCamera::GetSingleton();
		auto cameraNode = camera->cameraRoot.get()->AsNode();

		auto cameraNI = (RE::NiCamera*)((cameraNode->children.size() == 0) ? nullptr : cameraNode->children[0].get());
		if (!cameraNI) return;

		auto firstpersonNode = player->Get3D(1)->AsNode();
		auto thirdpersonNode = player->Get3D(0)->AsNode();
		auto headNode = Helper::GetHeadNode(thirdpersonNode);

		RE::NiPoint3 point1{}, point2{};
		ScalePoint(&point1, thirdpersonNode->world.scale);
		Utils::MatrixVectorMultiply(&point2, &thirdpersonNode->world.rotate, &point1);
		float headRot = 0.0f;

		if (GetHeadRotation(&headRot) || m_IsFakeCamera)
		{
			if (HeadRotation())
			{
				if (m_ICamera->GetID() == RE::CameraStates::kThirdPerson && m_ICamera->GetStateID() == CameraThirdPerson::State::kWerewolfIdle)
				{
					headNode = thirdpersonNode->GetObjectByName("Camera3rd [Cam3]")->AsNode();
					if (player->AsActorState()->IsSprinting())
						point1.z += -48.0f * thirdpersonNode->world.scale;
					else if (player->AsActorState()->IsRunning())
						point1.y += 12.0f * thirdpersonNode->world.scale;
				}
				Utils::MatrixVectorMultiply(&point2, &headNode->world.rotate, &point1);
			}
			cameraNI->world.translate = cameraNode->world.translate = cameraNode->local.translate = headNode->world.translate + point2;
		}
		// Fix casting magic from the correct position - FIXME
		if (player->AsActorState()->IsWeaponDrawn() && m_pluginConfig->Headbob().bCombat)
		{
			auto lefthandMagicNode = firstpersonNode->GetObjectByName("NPC L MagicNode [LMag]")->AsNode();
			if (lefthandMagicNode)
			{

			}
			auto righthandMagicNode = firstpersonNode->GetObjectByName("NPC R MagicNode [RMag]")->AsNode();
			if (righthandMagicNode)
			{

			}
		}
		// Fix shooting arrows from the correct position
		if (UseThirdPersonArms() || m_pluginConfig->Headbob().bCombat)
		{
			auto weaponNode = firstpersonNode->GetObjectByName("WEAPON")->AsNode();
			if (weaponNode)
			{
				weaponNode->world.translate = cameraNI->world.translate;
				weaponNode->world.translate.z -= m_pluginConfig->Camera().fFirstPersonCombatPosZ;
			}
		}
		// For floating quest markers
		Helper::UpdateNode((RE::NiNode*)cameraNI, RE::NiUpdateData::Flag::kDirty);
	}

	void ImprovedCameraSE::TranslateFirstPerson()
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		auto firstpersonNode = player->Get3D(1)->AsNode();
		auto thirdpersonNode = player->Get3D(0)->AsNode();
		auto cameraNode = RE::PlayerCamera::GetSingleton()->cameraRoot->AsNode();
		auto head = Helper::GetHeadNode(thirdpersonNode);
		RE::NiPoint3 vFirstPerson{}, vThirdPerson{}, vTransformRoot{}, point1{}, point2{};

		vFirstPerson = cameraNode->world.translate - firstpersonNode->world.translate;

		ScalePoint(&point1, thirdpersonNode->world.scale);
		Utils::MatrixVectorMultiply(&point2, &thirdpersonNode->world.rotate, &point1);

		vThirdPerson = (head->world.translate + point2) - thirdpersonNode->world.translate;
		vTransformRoot = thirdpersonNode->local.translate - firstpersonNode->local.translate;
		firstpersonNode->local.translate += vThirdPerson - vFirstPerson + vTransformRoot;
	}

	void ImprovedCameraSE::TranslateThirdPerson()
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		auto thirdpersonNode = player->Get3D(0)->AsNode();
		auto headNode = Helper::GetHeadNode(thirdpersonNode);
		auto cameraNode = RE::PlayerCamera::GetSingleton()->cameraRoot->AsNode();

		RE::NiPoint3 point1{}, point2{};
		float headRot = 0.0f;

		if (m_CurrentCameraID == RE::CameraStates::kFirstPerson ||
			m_CurrentCameraID == RE::CameraStates::kTween && m_PreviousCameraID == RE::CameraStates::kFirstPerson)
		{
			if (!GetHeadRotation(&headRot) && !m_IsFakeCamera)
			{
				if (player->AsActorState()->IsWeaponDrawn())
				{
					point1.x = m_pluginConfig->Camera().fFirstPersonCombatPosX;
					point1.y = m_pluginConfig->Camera().fFirstPersonCombatPosY;
					point1.z = m_pluginConfig->Camera().fFirstPersonCombatPosZ;
				}
				else
				{
					point1.x = m_pluginConfig->Camera().fFirstPersonPosX;
					point1.y = m_pluginConfig->Camera().fFirstPersonPosY;
					point1.z = m_pluginConfig->Camera().fFirstPersonPosZ;
				}

				Utils::MatrixVectorMultiply(&point2, &thirdpersonNode->world.rotate, &point1);
				thirdpersonNode->local.translate += cameraNode->world.translate - (headNode->world.translate + point2);
			}
			else
			{
				if (player->AsActorState()->IsSprinting() && !player->AsActorState()->IsSneaking())
					point1.y = -25.0f * thirdpersonNode->world.scale;

				else if (Helper::IsSittingOrSleeping(player))
					point1.y = m_pluginConfig->Camera().fFirstPersonPosY * thirdpersonNode->world.scale;

				if (player->AsActorState()->IsWeaponDrawn())
					point1.y -= m_pluginConfig->Camera().fFirstPersonCombatPosY * thirdpersonNode->world.scale;
				else
					point1.y -= m_pluginConfig->Camera().fFirstPersonPosY * thirdpersonNode->world.scale;

				Utils::MatrixVectorMultiply(&point2, &thirdpersonNode->world.rotate, &point1);
				thirdpersonNode->local.translate += point2;
			}
		}
	}

	bool ImprovedCameraSE::GetHeadRotation(float* rotation)
	{
		auto playerState = RE::PlayerCharacter::GetSingleton()->AsActorState();

		if (playerState->IsWeaponDrawn())
		{
			if (m_pluginConfig->Headbob().bCombat)
			{
				*rotation = m_pluginConfig->Headbob().fRotationCombat;
				return true;
			}
		}
		else
		{
			if (m_pluginConfig->Headbob().bSneakRoll && playerState->IsSneaking() && playerState->IsSprinting())
			{
				*rotation = m_pluginConfig->Headbob().fRotationSneakRoll;
				return true;
			}
			else if (m_pluginConfig->Headbob().bSneak && playerState->IsSneaking())
			{
				*rotation = m_pluginConfig->Headbob().fRotationSneak;
				return true;
			}
			else if (m_pluginConfig->Headbob().bSprint && playerState->IsSprinting())
			{
				*rotation = m_pluginConfig->Headbob().fRotationSprint;
				return true;
			}
			else if (m_pluginConfig->Headbob().bWalk && playerState->IsWalking())
			{
				*rotation = m_pluginConfig->Headbob().fRotationWalk;
				return true;
			}
			else if (m_pluginConfig->Headbob().bRun && playerState->IsRunning())
			{
				*rotation = m_pluginConfig->Headbob().fRotationRun;
				return true;
			}
			else if (m_pluginConfig->Headbob().bIdle && !playerState->IsWalking() && !playerState->IsRunning() && !playerState->IsSprinting()
				&& !playerState->IsSneaking() && !playerState->IsSwimming())
			{
				*rotation = m_pluginConfig->Headbob().fRotationIdle;
				return true;
			}
		}
		return false;
	}

	bool ImprovedCameraSE::HeadRotation()
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		auto camera = RE::PlayerCamera::GetSingleton();
		auto firstpersonState = (RE::FirstPersonState*)camera->cameraStates[RE::CameraState::kFirstPerson].get();
		auto thirdpersonNode = player->Get3D(0)->AsNode();
		auto headNode = Helper::GetHeadNode(thirdpersonNode);
		auto cameraNode = camera->cameraRoot.get()->AsNode();

		auto cameraNI = (RE::NiCamera*)((cameraNode->children.size() == 0) ? nullptr : cameraNode->children[0].get());
		if (!cameraNI) return false;

		RE::NiPoint3 point{}, angle{};;
		RE::NiMatrix3 matrix1{}, matrix2{};
		RE::NiQuaternion quat1{}, quat2{}, rQuat{};
		float headRot = 0.0f;

		firstpersonState->firstPersonCameraObj->world.rotate.ToEulerAnglesXYZ(angle);
		point.x = -player->data.angle.x - angle.x; // Pitch
		point.y = 0.5f * M_PI; // Yaw
		point.z = 0.5f * M_PI; // Roll

		Utils::EulerToMatrix(&matrix1, point.x, point.y, point.z);

		if (m_ICamera->GetID() == RE::CameraState::kFirstPerson && m_ICamera->GetStateID() == CameraFirstPerson::State::kSittingIdle)
			headNode->world.rotate = thirdpersonNode->world.rotate;

		if (!m_IsFakeCamera && GetHeadRotation(&headRot))
		{
			if (Helper::IsSittingOrSleeping(player))
				headRot = 0.0f;

			matrix2 = headNode->world.rotate * matrix1;

			Utils::RotMatrixToQuaternion(&cameraNI->world.rotate, &quat1);
			Utils::RotMatrixToQuaternion(&matrix2, &quat2);
			Utils::SlerpQuat(&rQuat, &quat1, &quat2, headRot);

			quat2 = rQuat;

			Utils::QuaternionToMatrix(&quat2, &cameraNI->world.rotate);
			Utils::MatrixInverse(&cameraNI->local.rotate, &matrix1);

			matrix2 = cameraNI->world.rotate * matrix1;
			cameraNode->world.rotate = cameraNode->local.rotate = matrix2;
		}
		else
		{
			auto thirdpersonState = (RE::ThirdPersonState*)camera->currentState.get();

			if (!thirdpersonState->IsInputEventHandlingEnabled() // Crafting
				|| camera->currentState->id == RE::CameraState::kVATS
				|| camera->currentState->id == RE::CameraState::kPCTransition
				|| camera->currentState->id == RE::CameraState::kAnimated
				|| camera->currentState->id == RE::CameraState::kBleedout
				|| m_ICamera->GetID() == RE::CameraState::kMount && m_ICamera->GetEventID() == CameraEvent::kHorseTransition
				|| m_ICamera->GetID() == RE::CameraState::kFirstPerson && m_ICamera->GetStateID() == CameraFirstPerson::State::kSleepingIdle)
			{
				// Clamp the direction for crafting.
				if (!thirdpersonState->IsInputEventHandlingEnabled())
				{
					if (point.x < -0.45f) { point.x = -0.45f; player->data.angle.x = 0.45f; }
					if (point.x > 0.0f) { point.x = 0.0f; player->data.angle.x = 0.0f; }
				}
				Utils::EulerToMatrix(&matrix1, point.x, point.y, point.z);

				cameraNI->world.rotate = headNode->world.rotate * matrix1;
			}
			// Scripted and Animation
			if (m_ICamera->GetID() == RE::CameraState::kThirdPerson &&
				(m_ICamera->GetStateID() == CameraThirdPerson::State::kScriptedIdle || m_ICamera->GetStateID() == CameraThirdPerson::State::kAnimationIdle))
			{
				point.x = 0.5f * M_PI - player->data.angle.x; // Pitch
				point.y = 0.5f * M_PI - thirdpersonState->freeRotation.x; // Yaw
				point.z = thirdpersonState->freeRotation.y; // Roll

				Utils::EulerToMatrix(&matrix1, point.x, point.y, point.z);
				cameraNI->world.rotate = headNode->world.rotate * matrix1;
			}
			Utils::MatrixInverse(&cameraNI->local.rotate, &matrix2);
			matrix1 = cameraNI->world.rotate * matrix2;
			cameraNode->world.rotate = cameraNode->local.rotate = matrix1;

			return true;
		}
		return false;
	}

	void ImprovedCameraSE::ScalePoint(RE::NiPoint3* point, float scale)
	{
		if (!m_ICamera)
			return;

		if (m_ICamera->GetID() == RE::CameraStates::kThirdPerson && m_ICamera->GetStateID() == CameraThirdPerson::State::kScriptedIdle)
		{
			point->x = m_pluginConfig->Camera().fScriptedPosX * scale;
			point->y = m_pluginConfig->Camera().fScriptedPosY * scale;
			point->z = m_pluginConfig->Camera().fScriptedPosZ * scale;
		}
		else if (m_ICamera->GetID() == RE::CameraStates::kThirdPerson && m_ICamera->GetStateID() == CameraThirdPerson::State::kNecroLichIdle)
		{
			point->x = m_pluginConfig->Camera().fNecroLichPosX * scale;
			point->y = m_pluginConfig->Camera().fNecroLichPosY * scale;
			point->z = m_pluginConfig->Camera().fNecroLichPosZ * scale;
		}
		else if (m_ICamera->GetID() == RE::CameraStates::kThirdPerson && m_ICamera->GetStateID() == CameraThirdPerson::State::kWerewolfIdle)
		{
			point->x = m_pluginConfig->Camera().fWerewolfPosX * scale;
			point->y = (m_pluginConfig->Camera().fWerewolfPosY + 48.0f) * scale;
			point->z = (m_pluginConfig->Camera().fWerewolfPosZ - 16.0f) * scale;
		}
		else if (m_ICamera->GetID() == RE::CameraStates::kThirdPerson && m_ICamera->GetStateID() == CameraThirdPerson::State::kVampireLordIdle)
		{
			point->x = m_pluginConfig->Camera().fVampireLordPosX * scale;
			point->y = m_pluginConfig->Camera().fVampireLordPosY * scale;
			point->z = m_pluginConfig->Camera().fVampireLordPosZ * scale;
		}
		else if (m_ICamera->GetID() == RE::CameraStates::kDragon && m_ICamera->GetStateID() == CameraDragon::State::kRiding)
		{
			point->x = m_pluginConfig->Camera().fDragonPosX * scale;
			point->y = m_pluginConfig->Camera().fDragonPosY * scale;
			point->z = m_pluginConfig->Camera().fDragonPosZ * scale;
		}
		else if (m_ICamera->GetID() == RE::CameraStates::kMount && m_ICamera->GetStateID() == CameraHorse::State::kWeaponDrawnIdle)
		{
			point->x = m_pluginConfig->Camera().fHorseCombatPosX * scale;
			point->y = m_pluginConfig->Camera().fHorseCombatPosY * scale;
			point->z = m_pluginConfig->Camera().fHorseCombatPosZ * scale;
		}
		else if (m_ICamera->GetID() == RE::CameraStates::kMount && m_ICamera->GetStateID() == CameraHorse::State::kRiding)
		{
			point->x = m_pluginConfig->Camera().fHorsePosX * scale;
			point->y = m_pluginConfig->Camera().fHorsePosY * scale;
			point->z = m_pluginConfig->Camera().fHorsePosZ * scale;
		}
		else if (m_ICamera->GetID() == RE::CameraStates::kFirstPerson && m_ICamera->GetStateID() == CameraFirstPerson::State::kWeaponDrawnIdle)
		{
			point->x = m_pluginConfig->Camera().fFirstPersonCombatPosX * scale;
			point->y = m_pluginConfig->Camera().fFirstPersonCombatPosY * scale;
			point->z = m_pluginConfig->Camera().fFirstPersonCombatPosZ * scale;
		}
		else
		{
			point->x = m_pluginConfig->Camera().fFirstPersonPosX * scale;
			point->y = m_pluginConfig->Camera().fFirstPersonPosY * scale;
			point->z = m_pluginConfig->Camera().fFirstPersonPosZ * scale;
		}
	}

	void ImprovedCameraSE::ReleaseAPIs()
	{
		if (m_SmoothCamAPI)
			m_SmoothCamAPI->ReleaseCameraControl(m_ICHandle);

		if (m_TDMAPI)
		{
			m_TDMAPI->ReleaseDisableHeadtracking(m_ICHandle);

			if (m_TDMSnapshot)
			{
				m_TDMSnapshot = false;
				*m_directionalMovementSheathed = m_directionalMovementSheathedOriginal;
				*m_directionalMovementDrawn = m_directionalMovementDrawnOriginal;
			}
		}
	}

	void ImprovedCameraSE::SetupCameraData()
	{
		m_Camera.Register(new CameraDeathCinematic);
		m_Camera.Register(new CameraFurniture);
		m_Camera.Register(new CameraTransition);
		m_Camera.Register(new CameraThirdPerson);
		m_Camera.Register(new CameraHorse);
		m_Camera.Register(new CameraRagdoll);
		m_Camera.Register(new CameraDragon);
		m_Camera.Register(new CameraFirstPerson);
	}
	
}
