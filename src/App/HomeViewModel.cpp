﻿#include "pch.h"
#include "HomeViewModel.h"
#if __has_include("HomeViewModel.g.cpp")
#include "HomeViewModel.g.cpp"
#endif
#include "AppSettings.h"
#include "MagService.h"


namespace winrt::Magpie::App::implementation {

HomeViewModel::HomeViewModel() {
	MagService& magService = MagService::Get();

	_isRunningChangedRevoker = magService.IsRunningChanged(
		auto_revoke, { this, &HomeViewModel::_MagService_IsRunningChanged });
	_isCountingDownRevoker = magService.IsCountingDownChanged(
		auto_revoke, { this, &HomeViewModel::_MagService_IsCountingDownChanged });
	_countdownTickRevoker = magService.CountdownTick(
		auto_revoke, { this, &HomeViewModel::_MagService_CountdownTick });
}

bool HomeViewModel::IsCountingDown() const noexcept {
	return MagService::Get().IsCountingDown();
}

float HomeViewModel::CountdownProgressRingValue() const noexcept {
	MagService& magService = MagService::Get();
	return magService.IsCountingDown() ? magService.CountdownLeft() / magService.TickingDownCount() : 1.0f;
}

hstring HomeViewModel::CountdownLabelText() const noexcept {
	MagService& magService = MagService::Get();
	return to_hstring((int)std::ceil(magService.CountdownLeft()));
}

hstring HomeViewModel::CountdownButtonText() const noexcept {
	MagService& magService = MagService::Get();
	if (magService.IsCountingDown()) {
		return L"取消";
	} else {
		return hstring(fmt::format(L"{} 秒后缩放", AppSettings::Get().DownCount()));
	}
}

bool HomeViewModel::IsNotRunning() const noexcept {
	return !MagService::Get().IsRunning();
}

void HomeViewModel::ToggleCountdown() const noexcept {
	MagService& magService = MagService::Get();
	if (magService.IsCountingDown()) {
		magService.StopCountdown();
	} else {
		magService.StartCountdown();
	}
}

uint32_t HomeViewModel::DownCount() const noexcept {
	return AppSettings::Get().DownCount();
}

void HomeViewModel::DownCount(uint32_t value) noexcept {
	AppSettings& settings = AppSettings::Get();

	if (settings.DownCount() == value) {
		return;
	}

	settings.DownCount(value);
	_propertyChangedEvent(*this, PropertyChangedEventArgs(L"DownCount"));
	_propertyChangedEvent(*this, PropertyChangedEventArgs(L"CountdownButtonText"));
}

void HomeViewModel::_MagService_IsCountingDownChanged(bool value) {
	if (!value) {
		_propertyChangedEvent(*this, PropertyChangedEventArgs(L"CountdownProgressRingValue"));
	}

	_propertyChangedEvent(*this, PropertyChangedEventArgs(L"CountdownProgressRingValue"));
	_propertyChangedEvent(*this, PropertyChangedEventArgs(L"CountdownLabelText"));
	_propertyChangedEvent(*this, PropertyChangedEventArgs(L"CountdownButtonText"));
	_propertyChangedEvent(*this, PropertyChangedEventArgs(L"IsCountingDown"));
}

void HomeViewModel::_MagService_CountdownTick(float) {
	_propertyChangedEvent(*this, PropertyChangedEventArgs(L"CountdownProgressRingValue"));
	_propertyChangedEvent(*this, PropertyChangedEventArgs(L"CountdownLabelText"));
}

void HomeViewModel::_MagService_IsRunningChanged(bool value) {
	_propertyChangedEvent(*this, PropertyChangedEventArgs(L"IsNotRunning"));
}

}
