// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/boxes/download_boost_box.h"

#include "ayu/ayu_settings.h"
#include "base/timer.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "storage/download_manager_mtproto.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <algorithm>

namespace AyuUi {
namespace {

constexpr auto kMinStartSessions = 1;
constexpr auto kMaxStartSessions = 12;
constexpr auto kMinMaxSessions = 1;
constexpr auto kMaxMaxSessions = 16;
constexpr auto kMinWindowParts = 4;
constexpr auto kMaxWindowParts = 32;
constexpr auto kMinVideoParts = 4;
constexpr auto kMaxVideoParts = 128;

[[nodiscard]] QString FormatWindowParts(int parts) {
	const auto mb = parts * 0.125;
	const auto kb = parts * 128;
	const auto mbText = ((kb % 512) == 0)
		? QString::number(mb, 'f', 1)
		: QString::number(mb, 'f', 2);
	return u"%1 MB（%2 片）"_q.arg(mbText).arg(parts);
}

[[nodiscard]] QString FormatLiveStatus(
		const std::vector<Storage::DownloadManagerMtproto::DcStatus> &statuses) {
	if (statuses.empty()) {
		return u"实时状态：当前无活跃下载任务"_q;
	}
	QStringList lines;
	lines.reserve(int(statuses.size()) + 1);
	lines.push_back(u"实时下载状态（自动刷新）："_q);
	for (const auto &dc : statuses) {
		const auto inFlightMb = dc.inFlightBytes / (1024.0 * 1024.0);
		if (dc.cooldownSecondsLeft > 0) {
			lines.push_back(
				u"DC%1：%2/%3 路  在途 %4 MB  ⚠ -429 冷却中（剩余 %5s，累计 %6 次）"_q
					.arg(dc.dcId)
					.arg(dc.activeSessions)
					.arg(dc.maxSessions)
					.arg(inFlightMb, 0, 'f', 1)
					.arg(dc.cooldownSecondsLeft)
					.arg(dc.floodCount));
		} else if (dc.floodCount > 0) {
			lines.push_back(
				u"DC%1：%2/%3 路  在途 %4 MB  正常（累计 -429：%5 次）"_q
					.arg(dc.dcId)
					.arg(dc.activeSessions)
					.arg(dc.maxSessions)
					.arg(inFlightMb, 0, 'f', 1)
					.arg(dc.floodCount));
		} else {
			lines.push_back(
				u"DC%1：%2/%3 路  在途 %4 MB  正常"_q
					.arg(dc.dcId)
					.arg(dc.activeSessions)
					.arg(dc.maxSessions)
					.arg(inFlightMb, 0, 'f', 1));
		}
	}
	return lines.join(u"\n"_q);
}

void FillDownloadBoostBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(rpl::single(u"下载加速"_q));
	box->setWidth(st::boxWideWidth);

	const auto container = box->verticalLayout();
	const auto session = &controller->session();

	struct State {
		bool syncing = false;
		rpl::event_stream<int> startLabel;
		rpl::event_stream<int> maxLabel;
		rpl::event_stream<int> windowLabel;
		rpl::event_stream<int> videoLabel;
		rpl::event_stream<bool> backoffToggle;
		Ui::MediaSlider *startSlider = nullptr;
		Ui::MediaSlider *maxSlider = nullptr;
		Ui::MediaSlider *windowSlider = nullptr;
		Ui::MediaSlider *videoSlider = nullptr;
		Ui::FlatLabel *statusLabel = nullptr;
		Ui::RpWidget *statusCard = nullptr;
	};
	const auto state = box->lifetime().make_state<State>();

	// Top live status card.
	const auto card = container->add(
		object_ptr<Ui::RpWidget>(container),
		st::boxRowPadding);
	state->statusCard = card;
	const auto statusLabel = Ui::CreateChild<Ui::FlatLabel>(
		card,
		st::boxDividerLabel);
	state->statusLabel = statusLabel;
	constexpr auto kCardPadX = 12;
	constexpr auto kCardPadY = 10;

	const auto updateCardLayout = [=] {
		const auto available = std::max(1, card->width() - kCardPadX * 2);
		statusLabel->resizeToWidth(available);
		statusLabel->moveToLeft(kCardPadX, kCardPadY);
		card->resize(card->width(), statusLabel->height() + kCardPadY * 2);
		card->update();
	};
	card->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(card);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(
			card->rect(),
			st::roundRadiusLarge,
			st::roundRadiusLarge);
	}, card->lifetime());
	card->widthValue(
	) | rpl::on_next([=](int) {
		updateCardLayout();
	}, card->lifetime());

	const auto refreshStatus = [=] {
		statusLabel->setText(
			FormatLiveStatus(session->downloader().liveStatus()));
		updateCardLayout();
	};
	refreshStatus();

	const auto statusTimer = box->lifetime().make_state<base::Timer>([=] {
		refreshStatus();
	});
	statusTimer->callEach(750);

	const auto syncUi = [=] {
		state->syncing = true;
		const auto &s = AyuSettings::getInstance();
		const auto start = s.downloadStartSessions();
		const auto max = s.downloadMaxSessions();
		const auto win = s.downloadMaxWindowParts();
		const auto vid = s.downloadVideoMaxParts();
		const auto backoff = s.downloadFloodBackoffEnabled();

		state->startLabel.fire_copy(start);
		state->maxLabel.fire_copy(max);
		state->windowLabel.fire_copy(win);
		state->videoLabel.fire_copy(vid);
		state->backoffToggle.fire_copy(backoff);

		if (state->startSlider) {
			state->startSlider->setValue(
				(start - kMinStartSessions)
				/ float64(kMaxStartSessions - kMinStartSessions));
		}
		if (state->maxSlider) {
			state->maxSlider->setValue(
				(max - kMinMaxSessions)
				/ float64(kMaxMaxSessions - kMinMaxSessions));
		}
		if (state->windowSlider) {
			state->windowSlider->setValue(
				(win - kMinWindowParts)
				/ float64(kMaxWindowParts - kMinWindowParts));
		}
		if (state->videoSlider) {
			state->videoSlider->setValue(
				(vid - kMinVideoParts)
				/ float64(kMaxVideoParts - kMinVideoParts));
		}
		state->syncing = false;
	};

	// Presets row.
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"一键预设"_q));

	const auto presetsRow = container->add(
		object_ptr<Ui::RpWidget>(container),
		st::boxRowPadding);
	const auto presetSafe = Ui::CreateChild<Ui::RoundButton>(
		presetsRow,
		rpl::single(u"稳妥防流控"_q),
		st::defaultLightButton);
	const auto presetBalanced = Ui::CreateChild<Ui::RoundButton>(
		presetsRow,
		rpl::single(u"均衡默认"_q),
		st::defaultLightButton);
	const auto presetTurbo = Ui::CreateChild<Ui::RoundButton>(
		presetsRow,
		rpl::single(u"极速模式"_q),
		st::defaultLightButton);

	presetsRow->resize(presetsRow->width(), st::defaultLightButton.height);
	presetsRow->widthValue(
	) | rpl::on_next([=](int width) {
		constexpr auto kGap = 8;
		const auto h = st::defaultLightButton.height;
		const auto btnW = std::max(1, (width - kGap * 2) / 3);
		presetSafe->setGeometry(0, 0, btnW, h);
		presetBalanced->setGeometry(btnW + kGap, 0, btnW, h);
		presetTurbo->setGeometry(
			(btnW + kGap) * 2,
			0,
			std::max(1, width - (btnW + kGap) * 2),
			h);
	}, presetsRow->lifetime());

	presetSafe->setClickedCallback([=] {
		AyuSettings::getInstance().setDownloadPreset(2, 6, 12, 48, true);
		syncUi();
		refreshStatus();
	});
	presetBalanced->setClickedCallback([=] {
		AyuSettings::getInstance().setDownloadPreset(6, 10, 24, 80, true);
		syncUi();
		refreshStatus();
	});
	presetTurbo->setClickedCallback([=] {
		AyuSettings::getInstance().setDownloadPreset(8, 16, 24, 128, true);
		syncUi();
		refreshStatus();
	});

	// Concurrency & window sliders.
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(u"并发与窗口参数"_q));

	const auto &initial = AyuSettings::getInstance();

	// 1. Start sessions (1..12)
	Settings::AddButtonWithLabel(
		container,
		rpl::single(u"单 DC 起步连接数"_q),
		state->startLabel.events_starting_with_copy(
			initial.downloadStartSessions()
		) | rpl::map([](int v) {
			return QString::number(v) + u" 路"_q;
		}),
		st::autoDownloadLimitButton
	)->setAttribute(Qt::WA_TransparentForMouseEvents);
	state->startSlider = container->add(
		object_ptr<Ui::MediaSlider>(container, st::autoDownloadLimitSlider),
		st::autoDownloadLimitPadding);
	state->startSlider->resize(st::autoDownloadLimitSlider.seekSize);
	state->startSlider->setPseudoDiscrete(
		kMaxStartSessions - kMinStartSessions + 1,
		[](int index) { return kMinStartSessions + index; },
		initial.downloadStartSessions(),
		[=](int value) {
			state->startLabel.fire_copy(value);
		},
		[=](int value) {
			if (state->syncing) {
				return;
			}
			AyuSettings::getInstance().setDownloadStartSessions(value);
			syncUi();
			refreshStatus();
		});

	// 2. Max sessions (1..16)
	Settings::AddButtonWithLabel(
		container,
		rpl::single(u"单 DC 最大连接数"_q),
		state->maxLabel.events_starting_with_copy(
			initial.downloadMaxSessions()
		) | rpl::map([](int v) {
			return QString::number(v) + u" 路"_q;
		}),
		st::autoDownloadLimitButton
	)->setAttribute(Qt::WA_TransparentForMouseEvents);
	state->maxSlider = container->add(
		object_ptr<Ui::MediaSlider>(container, st::autoDownloadLimitSlider),
		st::autoDownloadLimitPadding);
	state->maxSlider->resize(st::autoDownloadLimitSlider.seekSize);
	state->maxSlider->setPseudoDiscrete(
		kMaxMaxSessions - kMinMaxSessions + 1,
		[](int index) { return kMinMaxSessions + index; },
		initial.downloadMaxSessions(),
		[=](int value) {
			state->maxLabel.fire_copy(value);
		},
		[=](int value) {
			if (state->syncing) {
				return;
			}
			AyuSettings::getInstance().setDownloadMaxSessions(value);
			syncUi();
			refreshStatus();
		});

	// 3. Max window parts (4..32 -> 0.5 MB .. 4.0 MB)
	Settings::AddButtonWithLabel(
		container,
		rpl::single(u"单连接最大在途窗口"_q),
		state->windowLabel.events_starting_with_copy(
			initial.downloadMaxWindowParts()
		) | rpl::map([](int v) {
			return FormatWindowParts(v);
		}),
		st::autoDownloadLimitButton
	)->setAttribute(Qt::WA_TransparentForMouseEvents);
	state->windowSlider = container->add(
		object_ptr<Ui::MediaSlider>(container, st::autoDownloadLimitSlider),
		st::autoDownloadLimitPadding);
	state->windowSlider->resize(st::autoDownloadLimitSlider.seekSize);
	state->windowSlider->setPseudoDiscrete(
		kMaxWindowParts - kMinWindowParts + 1,
		[](int index) { return kMinWindowParts + index; },
		initial.downloadMaxWindowParts(),
		[=](int value) {
			state->windowLabel.fire_copy(value);
		},
		[=](int value) {
			if (state->syncing) {
				return;
			}
			AyuSettings::getInstance().setDownloadMaxWindowParts(value);
			syncUi();
			refreshStatus();
		});

	// 4. Video max in-flight parts (4..128)
	Settings::AddButtonWithLabel(
		container,
		rpl::single(u"单视频并发分片上限"_q),
		state->videoLabel.events_starting_with_copy(
			initial.downloadVideoMaxParts()
		) | rpl::map([](int v) {
			return QString::number(v) + u" 片"_q;
		}),
		st::autoDownloadLimitButton
	)->setAttribute(Qt::WA_TransparentForMouseEvents);
	state->videoSlider = container->add(
		object_ptr<Ui::MediaSlider>(container, st::autoDownloadLimitSlider),
		st::autoDownloadLimitPadding);
	state->videoSlider->resize(st::autoDownloadLimitSlider.seekSize);
	state->videoSlider->setPseudoDiscrete(
		kMaxVideoParts - kMinVideoParts + 1,
		[](int index) { return kMinVideoParts + index; },
		initial.downloadVideoMaxParts(),
		[=](int value) {
			state->videoLabel.fire_copy(value);
		},
		[=](int value) {
			if (state->syncing) {
				return;
			}
			AyuSettings::getInstance().setDownloadVideoMaxParts(value);
			syncUi();
			refreshStatus();
		});

	// -429 auto backoff toggle.
	Ui::AddSkip(container);
	const auto backoffButton = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(u"-429 自动降速退避"_q),
			st::settingsButtonNoIcon));
	backoffButton->toggleOn(state->backoffToggle.events_starting_with(
		initial.downloadFloodBackoffEnabled()));
	backoffButton->toggledChanges(
	) | rpl::on_next([=](bool checked) {
		if (state->syncing) {
			return;
		}
		if (checked != AyuSettings::getInstance().downloadFloodBackoffEnabled()) {
			AyuSettings::getInstance().setDownloadFloodBackoffEnabled(checked);
			refreshStatus();
		}
	}, backoffButton->lifetime());

	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::single(
			u"所有参数修改立即热生效并自动保存，无需重启，也不会中断正在进行的下载任务。\n"
			"连接数过高容易触发 Telegram -429 传输层流控，建议保持「-429 自动降速退避」开启。"_q));

	box->addLeftButton(rpl::single(u"恢复默认"_q), [=] {
		AyuSettings::getInstance().resetDownloadSettings();
		syncUi();
		refreshStatus();
	});
	box->addButton(rpl::single(u"关闭"_q), [=] {
		box->closeBox();
	});
}

} // namespace

void ShowDownloadBoostBox(not_null<Window::SessionController*> controller) {
	controller->show(Box(FillDownloadBoostBox, controller));
}

} // namespace AyuUi
