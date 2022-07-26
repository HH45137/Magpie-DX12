#pragma once

#include "ScalingProfilePage.g.h"
#include <winrt/Magpie.Runtime.h>


namespace winrt::Magpie::App::implementation {

struct ScalingProfilePage : ScalingProfilePageT<ScalingProfilePage> {
	ScalingProfilePage();

	void OnNavigatedTo(Navigation::NavigationEventArgs const& args);

	Magpie::App::ScalingProfileViewModel ViewModel() const noexcept {
		return _viewModel;
	}

	void ComboBox_DropDownOpened(IInspectable const& sender, IInspectable const&);

	void CursorScalingComboBox_SelectionChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&);

	Windows::Globalization::NumberFormatting::INumberFormatter2 NumberFormatter() const noexcept {
		return _numberFormatter;
	}

private:
	void _UpdateHeaderActionStyle();

	Magpie::App::ScalingProfileViewModel _viewModel{ nullptr };
	Windows::Globalization::NumberFormatting::DecimalFormatter _numberFormatter;

	Microsoft::UI::Xaml::Controls::NavigationView _rootNavigationView{ nullptr };
	Microsoft::UI::Xaml::Controls::NavigationView::DisplayModeChanged_revoker _displayModeChangedRevoker{};
};

}

namespace winrt::Magpie::App::factory_implementation {

struct ScalingProfilePage : ScalingProfilePageT<ScalingProfilePage, implementation::ScalingProfilePage> {
};

}