/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "menu/menu_item_download_files.h"

#include "base/base_file_utilities.h"
#include "base/flat_set.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_download_manager.h"
#include "data/data_file_click_handler.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history_inner_widget.h"
#include "history/history_item.h"
#include "history/view/history_view_list_widget.h" // HistoryView::SelectedItem.
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "storage/storage_account.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace Menu {
namespace {

using Documents = std::vector<std::pair<not_null<DocumentData*>, FullMsgId>>;
using Photos = std::vector<std::pair<not_null<PhotoData*>, FullMsgId>>;

[[nodiscard]] QString &LastBatchSaveFolder() {
	static auto result = QString();
	return result;
}

void CollectMedia(
		HistoryItem *item,
		Documents &documents,
		Photos &photos,
		base::flat_set<not_null<DocumentData*>> &seenDocs,
		base::flat_set<not_null<PhotoData*>> &seenPhotos) {
	if (!item || item->forbidsSaving()) {
		return;
	}
	const auto media = item->media();
	if (!media) {
		return;
	}
	if (const auto photo = media->photo()) {
		if (!photo->isNull() && seenPhotos.emplace(photo).second) {
			photos.emplace_back(photo, item->fullId());
		}
	} else if (const auto document = media->document()) {
		if (!document->isNull()
			&& !document->forbidsFileSave()
			&& seenDocs.emplace(document).second) {
			documents.emplace_back(document, item->fullId());
		}
	}
}

[[nodiscard]] QString DocumentFallbackExtension(
		not_null<DocumentData*> document) {
	if (document->isVoiceMessage()) {
		return document->hasMimeType(u"audio/mp3"_q)
			? u".mp3"_q
			: u".ogg"_q;
	} else if (document->isVideoMessage()) {
		return u".mp4"_q;
	} else if (const auto sticker = document->sticker()) {
		if (sticker->isLottie()) {
			return u".tgs"_q;
		} else if (sticker->isWebm()) {
			return u".webm"_q;
		}
		return u".webp"_q;
	}
	const auto mimeType = Core::MimeTypeForName(document->mimeString());
	const auto patterns = mimeType.globPatterns();
	if (!patterns.isEmpty()) {
		auto ext = patterns.front();
		ext.replace('*', QString());
		if (ext.startsWith('.') && ext.size() > 1) {
			return ext;
		}
	}
	if (document->isVideoFile() || document->isAnimation()) {
		return u".mp4"_q;
	} else if (document->isAudioFile()) {
		return u".mp3"_q;
	}
	return u".unknown"_q;
}

[[nodiscard]] QString DocumentFallbackPrefix(
		not_null<DocumentData*> document) {
	if (document->isVoiceMessage() || document->isAudioFile()) {
		return u"audio"_q;
	} else if (document->isVideoMessage()) {
		return u"round_video"_q;
	} else if (document->isVideoFile() || document->isAnimation()) {
		return u"video"_q;
	} else if (document->sticker()) {
		return u"sticker"_q;
	}
	return u"doc"_q;
}

[[nodiscard]] QString ResolveDocumentFileName(
		not_null<DocumentData*> document,
		TimeId fallbackDate,
		int index) {
	auto name = base::FileNameFromUserString(document->filename()).trimmed();
	while (name.endsWith('.') || name.endsWith(' ')) {
		name.chop(1);
	}
	const auto hasUsableName = !name.isEmpty()
		&& (name != u"."_q)
		&& (name != u".."_q)
		&& !name.startsWith('.');
	if (!hasUsableName) {
		const auto ext = (name.startsWith('.') && name.size() > 1)
			? name
			: DocumentFallbackExtension(document);
		const auto prefix = DocumentFallbackPrefix(document);
		const auto when = document->date
			? TimeId(document->date)
			: fallbackDate;
		if (when > 0) {
			const auto date = base::unixtime::parse(when);
			name = prefix + date.toString(u"_yyyy-MM-dd_HH-mm-ss"_q) + ext;
		} else {
			name = prefix + u"_%1"_q.arg(index) + ext;
		}
	}
#ifdef Q_OS_WIN
	const auto lower = name.trimmed().toLower();
	for (const auto &ext : { u".lnk"_q, u".scf"_q }) {
		if (lower.endsWith(ext)) {
			name += u".download"_q;
			break;
		}
	}
#endif // Q_OS_WIN
	return name;
}

[[nodiscard]] QString ResolvePhotoFileName(TimeId when, int index) {
	if (when > 0) {
		const auto date = base::unixtime::parse(when);
		return u"photo_%1_%2.jpg"_q
			.arg(index)
			.arg(date.toString(u"yyyy-MM-dd_HH-mm-ss"_q));
	}
	return u"photo_%1.jpg"_q.arg(index);
}

[[nodiscard]] QString ReserveUniqueFilePath(
		const QString &folderPath,
		const QString &fileName,
		base::flat_set<QString> &usedPaths) {
	const auto normalizedDir = folderPath.endsWith('/')
		? folderPath
		: (folderPath + '/');
	const auto extPos = fileName.lastIndexOf('.');
	const auto stem = (extPos > 0) ? fileName.left(extPos) : fileName;
	const auto ext = (extPos > 0) ? fileName.mid(extPos) : QString();
	const auto basePath = normalizedDir + stem;
	auto candidate = basePath + ext;
	for (auto index = 2;; ++index) {
		const auto key = candidate.toLower();
		if (!usedPaths.contains(key) && !QFileInfo::exists(candidate)) {
			usedPaths.emplace(key);
			return candidate;
		}
		candidate = basePath + u" (%1)"_q.arg(index) + ext;
	}
}

void ShowFolderSavedToast(
		not_null<Window::SessionController*> controller,
		const QString &lastPath,
		int photoCount,
		bool customFolder,
		bool isPhotosOnly) {
	if (lastPath.isEmpty()) {
		return;
	}
	const auto filter = [lastPath](const auto ...) {
		File::ShowInFolder(lastPath);
		return false;
	};
	const auto folderLabel = customFolder
		? QDir::toNativeSeparators(QFileInfo(lastPath).absolutePath())
		: tr::lng_mediaview_downloads(tr::now);
	const auto text = isPhotosOnly
		? (photoCount > 1
			? tr::lng_mediaview_saved_images_to
			: tr::lng_mediaview_saved_to)(
			tr::now,
			lt_downloads,
			tr::link(folderLabel, "internal:show_saved_message"),
			tr::marked)
		: tr::link(folderLabel, "internal:show_saved_message");
	controller->showToast({
		.text = text,
		.filter = filter,
		.iconLottie = u"toast/save_to_gallery"_q,
		.iconLottieSize = st::toastLottieIconSize,
		.st = &st::defaultToast,
	});
}

void AddAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		Documents &&documents,
		Photos &&photos,
		Fn<void()> callback) {
	const auto icon = documents.empty()
		? &st::menuIconSaveImage
		: &st::menuIconDownload;
	const auto shouldShowPhotoToast = documents.empty();

	const auto weak = base::make_weak(controller);
	const auto saveImages = [=](
			const QString &folderPath,
			const std::shared_ptr<base::flat_set<QString>> &usedPaths) {
		if (photos.empty()) {
			return;
		}
		const auto controller = weak.get();
		if (!controller) {
			return;
		}
		const auto session = &controller->session();
		const auto customFolder = !folderPath.isEmpty();
		const auto downloadPath = customFolder
			? folderPath
			: Core::App().settings().downloadPath();
		const auto path = downloadPath.isEmpty()
			? File::DefaultDownloadPath(session)
			: (downloadPath == FileDialog::Tmp())
			? session->local().tempDirectory()
			: downloadPath;
		if (path.isEmpty()) {
			return;
		}
		QDir().mkpath(path);

		auto views = std::vector<std::shared_ptr<Data::PhotoMedia>>();
		auto dates = std::vector<TimeId>();
		auto targetPaths = std::vector<QString>();
		views.reserve(photos.size());
		dates.reserve(photos.size());
		targetPaths.reserve(photos.size());
		auto photoIndex = 0;
		for (const auto &[photo, fullId] : photos) {
			if (const auto view = photo->createMediaView()) {
				view->wanted(Data::PhotoSize::Large, fullId);
				views.push_back(view);
				const auto photoDate = photo->date();
				const auto item = session->data().message(fullId);
				const auto when = photoDate
					? photoDate
					: (item ? item->date() : TimeId(0));
				dates.push_back(when);
				++photoIndex;
				targetPaths.push_back(ReserveUniqueFilePath(
					path,
					ResolvePhotoFileName(when, photoIndex),
					*usedPaths));
			}
		}

		const auto finalCheck = [=] {
			for (const auto &[photo, _] : photos) {
				if (photo->loading()) {
					return false;
				}
			}
			return true;
		};

		const auto saveToFiles = [=] {
			auto lastPath = QString();
			for (auto i = 0; i < int(views.size()); ++i) {
				const auto &filePath = targetPaths[i];
				if (views[i]->saveToFile(filePath)) {
					lastPath = filePath;
					if (dates[i] > 0) {
						auto f = QFile(filePath);
						if (f.open(QIODevice::ReadWrite)) {
							const auto when = base::unixtime::parse(dates[i]);
							f.setFileTime(
								when,
								QFileDevice::FileModificationTime);
							f.setFileTime(
								when,
								QFileDevice::FileAccessTime);
						}
					}
				}
			}
			if (shouldShowPhotoToast && !lastPath.isEmpty()) {
				if (const auto strong = weak.get()) {
					ShowFolderSavedToast(
						strong,
						lastPath,
						int(photos.size()),
						customFolder,
						true);
				}
			}
		};

		if (finalCheck()) {
			saveToFiles();
		} else {
			auto lifetime = std::make_shared<rpl::lifetime>();
			session->downloaderTaskFinished(
			) | rpl::on_next([=]() mutable {
				if (finalCheck()) {
					saveToFiles();
					base::take(lifetime)->destroy();
				}
			}, *lifetime);
		}
	};

	const auto saveDocuments = [=](
			const QString &folderPath,
			const std::shared_ptr<base::flat_set<QString>> &usedPaths) {
		if (documents.empty()) {
			return;
		}
		const auto controller = weak.get();
		if (!controller) {
			return;
		}
		const auto session = &controller->session();
		const auto customFolder = !folderPath.isEmpty();
		if (customFolder) {
			QDir().mkpath(folderPath);
		}
		auto docIndex = 0;
		auto anyLoading = false;
		auto lastImmediatePath = QString();
		for (const auto &[document, origin] : documents) {
			++docIndex;
			if (customFolder) {
				const auto item = session->data().message(origin);
				const auto fallbackDate = item ? item->date() : TimeId(0);
				const auto name = ResolveDocumentFileName(
					document,
					fallbackDate,
					docIndex);
				const auto targetPath = ReserveUniqueFilePath(
					folderPath,
					name,
					*usedPaths);
				const auto mediaView = document->createMediaView();
				document->save(origin, targetPath);
				if (document->loading()
					&& !document->loadingFilePath().isEmpty()) {
					anyLoading = true;
					if (item) {
						Core::App().downloadManager().addLoading({
							.item = item,
							.document = document,
						});
					}
				} else if (QFileInfo::exists(targetPath)) {
					lastImmediatePath = targetPath;
				}
			} else {
				DocumentSaveClickHandler::SaveAndTrack(origin, document);
			}
		}
		if (customFolder
			&& !anyLoading
			&& !lastImmediatePath.isEmpty()
			&& photos.empty()) {
			ShowFolderSavedToast(
				controller,
				lastImmediatePath,
				0,
				true,
				false);
		}
	};

	const auto save = [=](const QString &folderPath) {
		const auto usedPaths = std::make_shared<base::flat_set<QString>>();
		saveImages(folderPath, usedPaths);
		saveDocuments(folderPath, usedPaths);
		if (callback) {
			callback();
		}
	};

	const auto chooseFolderAndSave = [=] {
		const auto controller = weak.get();
		if (!controller) {
			return;
		}
		auto initialPath = LastBatchSaveFolder();
		if (initialPath.isEmpty() || !QDir(initialPath).exists()) {
			const auto settingsPath = Core::App().settings().downloadPath();
			if (!settingsPath.isEmpty() && settingsPath != FileDialog::Tmp()) {
				initialPath = settingsPath;
			}
		}
		while (initialPath.size() > 3 && initialPath.endsWith('/')) {
			initialPath.chop(1);
		}
		FileDialog::GetFolder(
			controller->window().widget().get(),
			tr::lng_download_path_choose(tr::now),
			initialPath,
			[=](const QString &result) {
				if (!result.isEmpty()) {
					LastBatchSaveFolder() = result;
					const auto folderPath = result.endsWith('/')
						? result
						: (result + '/');
					save(folderPath);
				}
			});
	};

	auto saveAsText = tr::lng_context_save_file(tr::now).trimmed();
	while (saveAsText.endsWith('.')
		|| saveAsText.endsWith(QChar(0x2026))
		|| saveAsText.endsWith(' ')) {
		saveAsText.chop(1);
	}
	if (saveAsText.isEmpty()) {
		saveAsText = tr::lng_context_save_file(tr::now);
	}

	if (Core::App().settings().askDownloadPath()) {
		menu->addAction(saveAsText, chooseFolderAndSave, icon);
	} else {
		const auto defaultText = documents.empty()
			? tr::lng_context_save_images_selected(tr::now)
			: tr::lng_context_save_documents_selected(tr::now);
		menu->addAction(defaultText, [=] {
			save(QString());
		}, icon);
		menu->addAction(saveAsText, chooseFolderAndSave, icon);
	}
}

void FinalizeAndAddAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		Documents &&docs,
		Photos &&photos,
		Fn<void()> onDone) {
	std::sort(docs.begin(), docs.end(), [](const auto &a, const auto &b) {
		return a.second < b.second;
	});
	std::sort(photos.begin(), photos.end(), [](const auto &a, const auto &b) {
		return a.second < b.second;
	});
	if (docs.empty() && photos.empty()) {
		return;
	}
	AddAction(
		menu,
		window,
		std::move(docs),
		std::move(photos),
		std::move(onDone));
}

} // namespace

void AddDownloadFilesAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		const std::vector<HistoryView::SelectedItem> &selectedItems,
		not_null<HistoryView::ListWidget*> list) {
	if (selectedItems.empty()) {
		return;
	}
	auto docs = Documents();
	auto photos = Photos();
	auto seenDocs = base::flat_set<not_null<DocumentData*>>();
	auto seenPhotos = base::flat_set<not_null<PhotoData*>>();
	for (const auto &selectedItem : selectedItems) {
		const auto item = window->session().data().message(selectedItem.msgId);
		CollectMedia(item, docs, photos, seenDocs, seenPhotos);
	}
	const auto done = [weak = base::make_weak(list)] {
		if (const auto strong = weak.get()) {
			strong->cancelSelection();
		}
	};
	FinalizeAndAddAction(
		menu,
		window,
		std::move(docs),
		std::move(photos),
		done);
}

void AddDownloadFilesAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		const std::vector<not_null<HistoryItem*>> &items,
		not_null<HistoryInner*> list) {
	if (items.empty()) {
		return;
	}
	auto docs = Documents();
	auto photos = Photos();
	auto seenDocs = base::flat_set<not_null<DocumentData*>>();
	auto seenPhotos = base::flat_set<not_null<PhotoData*>>();
	for (const auto &item : items) {
		CollectMedia(item, docs, photos, seenDocs, seenPhotos);
	}
	const auto done = [weak = base::make_weak(list)] {
		if (const auto strong = weak.get()) {
			strong->clearSelected();
		}
	};
	FinalizeAndAddAction(
		menu,
		window,
		std::move(docs),
		std::move(photos),
		done);
}

void AddDownloadFilesAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		const MessageIdsList &ids,
		Fn<void()> onDone) {
	if (ids.empty()) {
		return;
	}
	auto docs = Documents();
	auto photos = Photos();
	auto seenDocs = base::flat_set<not_null<DocumentData*>>();
	auto seenPhotos = base::flat_set<not_null<PhotoData*>>();
	for (const auto &id : ids) {
		const auto item = window->session().data().message(id);
		CollectMedia(item, docs, photos, seenDocs, seenPhotos);
	}
	FinalizeAndAddAction(
		menu,
		window,
		std::move(docs),
		std::move(photos),
		std::move(onDone));
}

} // namespace Menu
