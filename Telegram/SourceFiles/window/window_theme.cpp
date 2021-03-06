/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

In addition, as a special exception, the copyright holders give permission
to link the code of portions of this program with the OpenSSL library.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014-2017 John Preston, https://desktop.telegram.org
*/
#include "stdafx.h"
#include "window/window_theme.h"

#include "mainwidget.h"
#include "localstorage.h"
#include "core/parse_helper.h"
#include "core/zlib_help.h"
#include "styles/style_widgets.h"
#include "styles/style_history.h"

namespace Window {
namespace Theme {
namespace {

constexpr int kThemeFileSizeLimit = 5 * 1024 * 1024;
constexpr int kThemeBackgroundSizeLimit = 4 * 1024 * 1024;
constexpr int kThemeSchemeSizeLimit = 1024 * 1024;

constexpr int kMinimumTiledSize = 512;

struct Data {
	struct Applying {
		QString path;
		QByteArray content;
		QByteArray paletteForRevert;
		Cached cached;
	};

	ChatBackground background;
	Applying applying;
};
NeverFreedPointer<Data> instance;

QByteArray readThemeContent(const QString &path) {
	QFile file(path);
	if (!file.exists()) {
		LOG(("Error: theme file not found: %1").arg(path));
		return QByteArray();
	}

	if (file.size() > kThemeFileSizeLimit) {
		LOG(("Error: theme file too large: %1 (should be less than 5 MB, got %2)").arg(path).arg(file.size()));
		return QByteArray();
	}
	if (!file.open(QIODevice::ReadOnly)) {
		LOG(("Warning: could not open theme file: %1").arg(path));
		return QByteArray();
	}

	return file.readAll();
}

inline uchar readHexUchar(char code, bool &error) {
	if (code >= '0' && code <= '9') {
		return ((code - '0') & 0xFF);
	} else if (code >= 'a' && code <= 'f') {
		return ((code + 10 - 'a') & 0xFF);
	} else if (code >= 'A' && code <= 'F') {
		return ((code + 10 - 'A') & 0xFF);
	}
	error = true;
	return 0xFF;
}

inline uchar readHexUchar(char char1, char char2, bool &error) {
	return ((readHexUchar(char1, error) & 0x0F) << 4) | (readHexUchar(char2, error) & 0x0F);
}

bool readNameAndValue(const char *&from, const char *end, QLatin1String *outName, QLatin1String *outValue) {
	using base::parse::skipWhitespaces;
	using base::parse::readName;

	if (!skipWhitespaces(from, end)) return true;

	*outName = readName(from, end);
	if (outName->size() == 0) {
		LOG(("Error: Could not read name in the color scheme."));
		return false;
	}
	if (!skipWhitespaces(from, end)) {
		LOG(("Error: Unexpected end of the color scheme."));
		return false;
	}
	if (*from != ':') {
		LOG(("Error: Expected ':' between each name and value in the color scheme."));
		return false;
	}
	if (!skipWhitespaces(++from, end)) {
		LOG(("Error: Unexpected end of the color scheme."));
		return false;
	}
	auto valueStart = from;
	if (*from == '#') ++from;

	if (readName(from, end).size() == 0) {
		LOG(("Error: Expected a color value in #rrggbb or #rrggbbaa format in the color scheme."));
		return false;
	}
	*outValue = QLatin1String(valueStart, from - valueStart);

	if (!skipWhitespaces(from, end)) {
		LOG(("Error: Unexpected end of the color scheme."));
		return false;
	}
	if (*from != ';') {
		LOG(("Error: Expected ';' after each value in the color scheme."));
		return false;
	}
	++from;
	return true;
}

enum class SetResult {
	Ok,
	Bad,
	NotFound,
};
SetResult setColorSchemeValue(QLatin1String name, QLatin1String value, Instance *out) {
	auto found = false;
	auto size = value.size();
	auto data = value.data();
	if (data[0] == '#' && (size == 7 || size == 9)) {
		auto error = false;
		auto r = readHexUchar(data[1], data[2], error);
		auto g = readHexUchar(data[3], data[4], error);
		auto b = readHexUchar(data[5], data[6], error);
		auto a = (size == 9) ? readHexUchar(data[7], data[8], error) : uchar(255);
		if (error) {
			LOG(("Error: Expected a color value in #rrggbb or #rrggbbaa format in the color scheme (while applying '%1: %2')").arg(QLatin1String(name)).arg(QLatin1String(value)));
			return SetResult::Bad;
		} else if (out) {
			found = out->palette.setColor(name, r, g, b, a);
		} else {
			found = style::main_palette::setColor(name, r, g, b, a);
		}
	} else {
		if (out) {
			found = out->palette.setColor(name, value);
		} else {
			found = style::main_palette::setColor(name, value);
		}
	}
	return found ? SetResult::Ok : SetResult::NotFound;
}

bool loadColorScheme(const QByteArray &content, Instance *out = nullptr) {
	if (content.size() > kThemeSchemeSizeLimit) {
		LOG(("Error: color scheme file too large (should be less than 1 MB, got %2)").arg(content.size()));
		return false;
	}

	QMap<QLatin1String, QLatin1String> unsupported;
	auto data = base::parse::stripComments(content);
	auto from = data.constData(), end = from + data.size();
	while (from != end) {
		QLatin1String name(""), value("");
		if (!readNameAndValue(from, end, &name, &value)) {
			return false;
		}
		if (name.size() == 0) { // End of content reached.
			return true;
		}

		// Find the named value in the already read unsupported list.
		value = unsupported.value(value, value);

		auto result = setColorSchemeValue(name, value, out);
		if (result == SetResult::Bad) {
			return false;
		} else if (result == SetResult::NotFound) {
			LOG(("Warning: unexpected name or value in the color scheme (while applying '%1: %2')").arg(name).arg(value));
			unsupported.insert(name, value);
		}
	}
	return true;
}

void applyBackground(QImage &&background, bool tiled, Instance *out) {
	if (out) {
		out->background = std_::move(background);
		out->tiled = tiled;
	} else {
		Background()->setThemeData(std_::move(background), tiled);
	}
}

bool loadThemeFromCache(const QByteArray &content, Cached &cache) {
	if (cache.paletteChecksum != style::palette::Checksum()) {
		return false;
	}
	if (cache.contentChecksum != hashCrc32(content.constData(), content.size())) {
		return false;
	}

	QImage background;
	if (!cache.background.isEmpty()) {
		QBuffer buffer(&cache.background);
		QImageReader reader(&buffer);
#ifndef OS_MAC_OLD
		reader.setAutoTransform(true);
#endif // OS_MAC_OLD
		if (!reader.read(&background) || background.isNull()) {
			return false;
		}
	}

	if (!style::main_palette::load(cache.colors)) {
		return false;
	}
	if (!background.isNull()) {
		applyBackground(std_::move(background), cache.tiled, nullptr);
	}

	return true;
}

enum class LoadResult {
	Loaded,
	Failed,
	NotFound,
};

LoadResult loadBackgroundFromFile(zlib::FileToRead &file, const char *filename, QByteArray *outBackground) {
	*outBackground = file.readFileContent(filename, zlib::kCaseInsensitive, kThemeBackgroundSizeLimit);
	if (file.error() == UNZ_OK) {
		return LoadResult::Loaded;
	} else if (file.error() == UNZ_END_OF_LIST_OF_FILE) {
		file.clearError();
		return LoadResult::NotFound;
	}
	LOG(("Error: could not read '%1' in the theme file.").arg(filename));
	return LoadResult::Failed;
}

bool loadBackground(zlib::FileToRead &file, QByteArray *outBackground, bool *outTiled) {
	auto result = loadBackgroundFromFile(file, "background.jpg", outBackground);
	if (result != LoadResult::NotFound) return (result == LoadResult::Loaded);

	result = loadBackgroundFromFile(file, "background.png", outBackground);
	if (result != LoadResult::NotFound) return (result == LoadResult::Loaded);

	*outTiled = true;
	result = loadBackgroundFromFile(file, "tiled.jpg", outBackground);
	if (result != LoadResult::NotFound) return (result == LoadResult::Loaded);

	result = loadBackgroundFromFile(file, "tiled.png", outBackground);
	if (result != LoadResult::NotFound) return (result == LoadResult::Loaded);
	return true;
}

bool loadTheme(const QByteArray &content, Cached &cache, Instance *out = nullptr) {
	cache = Cached();
	zlib::FileToRead file(content);

	unz_global_info globalInfo = { 0 };
	file.getGlobalInfo(&globalInfo);
	if (file.error() == UNZ_OK) {
		auto schemeContent = file.readFileContent("colors.tdesktop-theme", zlib::kCaseInsensitive, kThemeSchemeSizeLimit);
		if (file.error() != UNZ_OK) {
			LOG(("Error: could not read 'colors.tdesktop-theme' in the theme file."));
			return false;
		}
		if (!loadColorScheme(schemeContent, out)) {
			return false;
		}

		auto backgroundTiled = false;
		auto backgroundContent = QByteArray();
		if (!loadBackground(file, &backgroundContent, &backgroundTiled)) {
			return false;
		}

		if (!backgroundContent.isEmpty()) {
			auto background = App::readImage(backgroundContent);
			if (background.isNull()) {
				LOG(("Error: could not read background image in the theme file."));
				return false;
			}
			QBuffer buffer(&cache.background);
			if (!background.save(&buffer, "BMP")) {
				LOG(("Error: could not write background image as a BMP to cache."));
				return false;
			}
			cache.tiled = backgroundTiled;

			applyBackground(std_::move(background), cache.tiled, out);
		}
	} else {
		// Looks like it is not a .zip theme.
		if (!loadColorScheme(content, out)) {
			return false;
		}
	}
	if (out) {
		cache.colors = out->palette.save();
	} else {
		cache.colors = style::main_palette::save();
	}
	cache.paletteChecksum = style::palette::Checksum();
	cache.contentChecksum = hashCrc32(content.constData(), content.size());

	return true;
}

QImage prepareBackgroundImage(QImage &&image) {
	if (image.format() != QImage::Format_ARGB32 && image.format() != QImage::Format_ARGB32_Premultiplied && image.format() != QImage::Format_RGB32) {
		image = std_::move(image).convertToFormat(QImage::Format_RGB32);
	}
	image.setDevicePixelRatio(cRetinaFactor());
	return std_::move(image);
}

void initColor(style::color color, float64 hue, float64 saturation) {
	auto original = color->c;
	original.setHslF(hue, saturation, original.lightnessF(), original.alphaF());
	color.set(original.red(), original.green(), original.blue(), original.alpha());
}

void initColorsFromBackground(const QImage &img) {
	t_assert(img.format() == QImage::Format_ARGB32_Premultiplied);

	uint64 components[3] = { 0 };
	uint64 componentsScroll[3] = { 0 };
	auto w = img.width();
	auto h = img.height();
	auto size = w * h;
	if (auto pix = img.constBits()) {
		for (auto i = 0, l = size * 4; i != l; i += 4) {
			components[2] += pix[i + 0];
			components[1] += pix[i + 1];
			components[0] += pix[i + 2];
		}
	}

	if (size) {
		for (auto i = 0; i != 3; ++i) {
			components[i] /= size;
		}
	}

	auto bgColor = QColor(components[0], components[1], components[2]);
	auto hue = bgColor.hslHueF();
	auto saturation = bgColor.hslSaturationF();
	initColor(st::msgServiceBg, hue, saturation);
	initColor(st::msgServiceBgSelected, hue, saturation);
	initColor(st::historyScroll.bg, hue, saturation);
	initColor(st::historyScroll.bgOver, hue, saturation);
	initColor(st::historyScroll.barBg, hue, saturation);
	initColor(st::historyScroll.barBgOver, hue, saturation);
}

} // namespace

void ChatBackground::setThemeData(QImage &&themeImage, bool themeTile) {
	_themeImage = prepareBackgroundImage(std_::move(themeImage));
	_themeTile = themeTile;
}

void ChatBackground::start() {
	if (_id == internal::kUninitializedBackground) {
		if (!Local::readBackground()) {
			setImage(kThemeBackground);
		}
	}
}

void ChatBackground::setImage(int32 id, QImage &&image) {
	if (id == kThemeBackground && _themeImage.isNull()) {
		id = kDefaultBackground;
	}
	_id = id;
	if (_id == kThemeBackground) {
		_tile = _themeTile;
		setPreparedImage(QImage(_themeImage));
	} else if (_id == internal::kTestingThemeBackground || _id == internal::kTestingDefaultBackground) {
		if (_id == internal::kTestingDefaultBackground || image.isNull()) {
			image.load(qsl(":/gui/art/bg.jpg"));
			_id = internal::kTestingDefaultBackground;
		}
		setPreparedImage(std_::move(image));
	} else {
		if (_id == kInitialBackground) {
			image.load(qsl(":/gui/art/bg_initial.png"));
			if (cRetina()) {
				image = image.scaledToWidth(image.width() * 2, Qt::SmoothTransformation);
			} else if (cScale() != dbisOne) {
				image = image.scaledToWidth(convertScale(image.width()), Qt::SmoothTransformation);
			}
		} else if (_id == kDefaultBackground || image.isNull()) {
			_id = kDefaultBackground;
			image.load(qsl(":/gui/art/bg.jpg"));
		}
		Local::writeBackground(_id, (_id == kDefaultBackground || _id == kInitialBackground) ? QImage() : image);
		setPreparedImage(prepareBackgroundImage(std_::move(image)));
	}
	t_assert(!_pixmap.isNull() && !_pixmapForTiled.isNull());
	notify(BackgroundUpdate(BackgroundUpdate::Type::New, _tile));
}

void ChatBackground::setPreparedImage(QImage &&image) {
	image = std_::move(image).convertToFormat(QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(cRetinaFactor());
	if (_id != kThemeBackground && _id != internal::kTestingThemeBackground) {
		auto colorsFromSomeTheme = Local::hasTheme();
		if (instance && !instance->applying.paletteForRevert.isEmpty()) {
			colorsFromSomeTheme = !instance->applying.path.isEmpty();
		}
		if (colorsFromSomeTheme || (_id != kDefaultBackground && _id != internal::kTestingDefaultBackground)) {
			initColorsFromBackground(image);
		}
	}

	auto width = image.width();
	auto height = image.height();
	t_assert(width > 0 && height > 0);
	auto isSmallForTiled = (width < kMinimumTiledSize || height < kMinimumTiledSize);
	if (isSmallForTiled) {
		auto repeatTimesX = qCeil(kMinimumTiledSize / float64(width));
		auto repeatTimesY = qCeil(kMinimumTiledSize / float64(height));
		auto imageForTiled = QImage(width * repeatTimesX, height * repeatTimesY, QImage::Format_ARGB32_Premultiplied);
		imageForTiled.setDevicePixelRatio(image.devicePixelRatio());
		auto imageForTiledBytes = imageForTiled.bits();
		auto bytesInLine = width * sizeof(uint32);
		for (auto timesY = 0; timesY != repeatTimesY; ++timesY) {
			auto imageBytes = image.constBits();
			for (auto y = 0; y != height; ++y) {
				for (auto timesX = 0; timesX != repeatTimesX; ++timesX) {
					memcpy(imageForTiledBytes, imageBytes, bytesInLine);
					imageForTiledBytes += bytesInLine;
				}
				imageBytes += image.bytesPerLine();
				imageForTiledBytes += imageForTiled.bytesPerLine() - (repeatTimesX * bytesInLine);
			}
		}
		_pixmapForTiled = App::pixmapFromImageInPlace(std_::move(imageForTiled));
	}
	_pixmap = App::pixmapFromImageInPlace(std_::move(image));
	if (!isSmallForTiled) {
		_pixmapForTiled = _pixmap;
	}
}

int32 ChatBackground::id() const {
	return _id;
}

bool ChatBackground::tile() const {
	return _tile;
}

bool ChatBackground::tileForSave() const {
	if (_id == internal::kTestingThemeBackground ||
		_id == internal::kTestingDefaultBackground) {
		return _tileForRevert;
	}
	return tile();
}

void ChatBackground::ensureStarted() {
	if (_pixmap.isNull()) {
		// We should start first, otherwise the default call
		// to start() will reset this value to _themeTile.
		start();
	}
}

void ChatBackground::setTile(bool tile) {
	ensureStarted();
	if (_tile != tile) {
		_tile = tile;
		if (_id != internal::kTestingThemeBackground && _id != internal::kTestingDefaultBackground) {
			Local::writeUserSettings();
		}
		notify(BackgroundUpdate(BackgroundUpdate::Type::Changed, _tile));
	}
}

void ChatBackground::reset() {
	if (_id == internal::kTestingThemeBackground || _id == internal::kTestingDefaultBackground) {
		if (_themeImage.isNull()) {
			_idForRevert = kDefaultBackground;
			_imageForRevert = QImage();
			_tileForRevert = false;
		} else {
			_idForRevert = kThemeBackground;
			_imageForRevert = _themeImage;
			_tileForRevert = _themeTile;
		}
	} else {
		setImage(kThemeBackground);
	}
}

void ChatBackground::saveForRevert() {
	ensureStarted();
	if (_id != internal::kTestingThemeBackground && _id != internal::kTestingDefaultBackground) {
		_idForRevert = _id;
		_imageForRevert = std_::move(_pixmap).toImage();
		_tileForRevert = _tile;
	}
}

void ChatBackground::setTestingTheme(Instance &&theme) {
	style::main_palette::apply(theme.palette);
	if (!theme.background.isNull() || _id == kThemeBackground) {
		saveForRevert();
		setImage(internal::kTestingThemeBackground, std_::move(theme.background));
		setTile(theme.tiled);
	} else {
		// Apply current background image so that service bg colors are recounted.
		setImage(_id, std_::move(_pixmap).toImage());
	}
	notify(BackgroundUpdate(BackgroundUpdate::Type::TestingTheme, _tile), true);
}

void ChatBackground::setTestingDefaultTheme() {
	style::main_palette::reset();
	if (_id == kThemeBackground) {
		saveForRevert();
		setImage(internal::kTestingDefaultBackground);
		setTile(false);
	} else {
		// Apply current background image so that service bg colors are recounted.
		setImage(_id, std_::move(_pixmap).toImage());
	}
	notify(BackgroundUpdate(BackgroundUpdate::Type::TestingTheme, _tile), true);
}

void ChatBackground::keepApplied() {
	if (_id == internal::kTestingThemeBackground) {
		_id = kThemeBackground;
		_themeImage = _pixmap.toImage();
		_themeTile = _tile;
		writeNewBackgroundSettings();
	} else if (_id == internal::kTestingDefaultBackground) {
		_id = kDefaultBackground;
		_themeImage = QImage();
		_themeTile = false;
		writeNewBackgroundSettings();
	}
	notify(BackgroundUpdate(BackgroundUpdate::Type::ApplyingTheme, _tile), true);
}

void ChatBackground::writeNewBackgroundSettings() {
	if (_tile != _tileForRevert) {
		Local::writeUserSettings();
	}
	Local::writeBackground(_id, QImage());
}

void ChatBackground::revert() {
	if (_id == internal::kTestingThemeBackground || _id == internal::kTestingDefaultBackground) {
		setTile(_tileForRevert);
		setImage(_idForRevert, std_::move(_imageForRevert));
	} else {
		// Apply current background image so that service bg colors are recounted.
		setImage(_id, std_::move(_pixmap).toImage());
	}
	notify(BackgroundUpdate(BackgroundUpdate::Type::RevertingTheme, _tile), true);
}


ChatBackground *Background() {
	instance.createIfNull();
	return &instance->background;
}

bool Load(const QString &pathRelative, const QString &pathAbsolute, const QByteArray &content, Cached &cache) {
	if (content.size() < 4) {
		LOG(("Error: Could not load theme from '%1' (%2)").arg(pathRelative).arg(pathAbsolute));
		return false;
	}

	instance.createIfNull();
	if (loadThemeFromCache(content, cache)) {
		return true;
	}

	if (!loadTheme(content, cache)) {
		return false;
	}
	Local::writeTheme(pathRelative, pathAbsolute, content, cache);
	return true;
}

void Unload() {
	instance.clear();
}

bool Apply(const QString &filepath) {
	auto preview = std_::make_unique<Preview>();
	preview->path = filepath;
	if (!LoadFromFile(preview->path, &preview->instance, &preview->content)) {
		return false;
	}
	return Apply(std_::move(preview));
}

bool Apply(std_::unique_ptr<Preview> preview) {
	instance.createIfNull();
	instance->applying.path = std_::move(preview->path);
	instance->applying.content = std_::move(preview->content);
	instance->applying.cached = std_::move(preview->instance.cached);
	if (instance->applying.paletteForRevert.isEmpty()) {
		instance->applying.paletteForRevert = style::main_palette::save();
	}
	Background()->setTestingTheme(std_::move(preview->instance));
	return true;
}

void ApplyDefault() {
	instance.createIfNull();
	instance->applying.path = QString();
	instance->applying.content = QByteArray();
	instance->applying.cached = Cached();
	if (instance->applying.paletteForRevert.isEmpty()) {
		instance->applying.paletteForRevert = style::main_palette::save();
	}
	Background()->setTestingDefaultTheme();
}

void KeepApplied() {
	if (!instance) {
		return;
	}
	auto filepath = instance->applying.path;
	auto pathRelative = filepath.isEmpty() ? QString() : QDir().relativeFilePath(filepath);
	auto pathAbsolute = filepath.isEmpty() ? QString() : QFileInfo(filepath).absoluteFilePath();
	Local::writeTheme(pathRelative, pathAbsolute, instance->applying.content, instance->applying.cached);
	instance->applying = Data::Applying();
	Background()->keepApplied();
}

void Revert() {
	if (!instance->applying.paletteForRevert.isEmpty()) {
		style::main_palette::load(instance->applying.paletteForRevert);
	}
	instance->applying = Data::Applying();
	Background()->revert();
}

bool LoadFromFile(const QString &path, Instance *out, QByteArray *outContent) {
	*outContent = readThemeContent(path);
	if (outContent->size() < 4) {
		LOG(("Error: Could not load theme from %1").arg(path));
		return false;
	}

	return loadTheme(*outContent,  out->cached, out);
}

void ComputeBackgroundRects(QRect wholeFill, QSize imageSize, QRect &to, QRect &from) {
	if (uint64(imageSize.width()) * wholeFill.height() > uint64(imageSize.height()) * wholeFill.width()) {
		float64 pxsize = wholeFill.height() / float64(imageSize.height());
		int takewidth = qCeil(wholeFill.width() / pxsize);
		if (takewidth > imageSize.width()) {
			takewidth = imageSize.width();
		} else if ((imageSize.width() % 2) != (takewidth % 2)) {
			++takewidth;
		}
		to = QRect(int((wholeFill.width() - takewidth * pxsize) / 2.), 0, qCeil(takewidth * pxsize), wholeFill.height());
		from = QRect((imageSize.width() - takewidth) / 2, 0, takewidth, imageSize.height());
	} else {
		float64 pxsize = wholeFill.width() / float64(imageSize.width());
		int takeheight = qCeil(wholeFill.height() / pxsize);
		if (takeheight > imageSize.height()) {
			takeheight = imageSize.height();
		} else if ((imageSize.height() % 2) != (takeheight % 2)) {
			++takeheight;
		}
		to = QRect(0, int((wholeFill.height() - takeheight * pxsize) / 2.), wholeFill.width(), qCeil(takeheight * pxsize));
		from = QRect(0, (imageSize.height() - takeheight) / 2, imageSize.width(), takeheight);
	}
}

} // namespace Theme
} // namespace Window
