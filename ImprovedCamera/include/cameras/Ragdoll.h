/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "cameras/ICamera.h"


namespace ImprovedCamera {

	class CameraRagdoll : public Interface::ICamera {

	public:
		struct State
		{
			enum Ragdoll : std::uint8_t
			{
				kStopped = 0,
				kPlaying,
				kPlayingDeath,

				kTotal
			};
		};

	public:
		CameraRagdoll();
		virtual ~CameraRagdoll() = default;

		virtual bool OnUpdate(std::uint8_t currentCamera, std::uint8_t previousCamera) override;
		virtual bool HasControl() override;
		virtual std::uint8_t GetEventID() override;
		virtual std::uint8_t GetStateID() override { return m_RagdollState; };
		virtual const Data& GetData() const override { return this->m_Data; };
		virtual void OnShutdown() override {};

	private:
		void SetData();

		std::uint8_t m_RagdollState = 0;
	};

}
