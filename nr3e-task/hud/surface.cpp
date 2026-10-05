#include "a_native_window_creator.h"

#include <cstdint>

namespace android
{
	ANativeWindow* ANativeWindowCreator::Create(const CreateOptions& options)
	{
		auto& Client = GetComposerInstance();
		int32_t W = options.width;
		int32_t H = options.height;
		if (W == 0 || H == 0)
		{
			anative_window_creator::detail::types::ui::DisplayState Info{};
			if (Client.GetDisplayInfo(&Info))
			{
				W = Info.layerStackSpaceRect.width;
				H = Info.layerStackSpaceRect.height;
			}
		}
		auto Control = Client.CreateSurface(options.name, W, H, {}, options.skipScreenshot);
		auto* Window = reinterpret_cast<ANativeWindow*>(Control.GetSurface());
		if (!Window)
			return nullptr;
		m_cachedSurfaceControl.emplace(Window, std::move(Control));
		{
			using android::anative_window_creator::detail::compat::SystemVersion;
			auto& Cached = m_cachedSurfaceControl[Window];
			auto& Tx = Client.GetDefaultTransaction();
			auto& Sp = static_cast<anative_window_creator::detail::types::StrongPointer<void>&>(
				Cached);
			Tx.SetLayer(Sp, INT32_MAX);
			if (SystemVersion >= 12)
				Tx.SetTrustedOverlay(Sp, true);
			Tx.Show(Sp);
			Tx.Apply(false, true);
		}
		return Window;
	}
}
