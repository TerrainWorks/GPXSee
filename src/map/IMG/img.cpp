#include <QMap>
#include <QtEndian>
#include "common/programpaths.h"
#include "vectortile.h"
#include "img.h"

#define CACHED_SUBDIVS_COUNT 2048 // ~32MB

typedef QMap<QString, VectorTile*> TileMap;

struct PolyCTX
{
	PolyCTX(const RectC &rect, int bits, QList<IMG::Poly> *polygons,
	  QList<IMG::Poly> *lines, QCache<const SubDiv*, IMG::Polys> *polyCache)
	  : rect(rect), bits(bits), polygons(polygons), lines(lines),
	  polyCache(polyCache) {}

	const RectC &rect;
	int bits;
	QList<IMG::Poly> *polygons;
	QList<IMG::Poly> *lines;
	QCache<const SubDiv*, IMG::Polys> *polyCache;
};

struct PointCTX
{
	PointCTX(const RectC &rect, int bits, QList<IMG::Point> *points,
	  QCache<const SubDiv*, QList<IMG::Point> > *pointCache)
	  : rect(rect), bits(bits), points(points), pointCache(pointCache) {}

	const RectC &rect;
	int bits;
	QList<IMG::Point> *points;
	QCache<const SubDiv*, QList<IMG::Point> > *pointCache;
};

static SubFile::Type tileType(const char str[3])
{
	if (!memcmp(str, "TRE", 3))
		return SubFile::TRE;
	else if (!memcmp(str, "RGN", 3))
		return SubFile::RGN;
	else if (!memcmp(str, "LBL", 3))
		return SubFile::LBL;
	else if (!memcmp(str, "TYP", 3))
		return SubFile::TYP;
	else if (!memcmp(str, "GMP", 3))
		return SubFile::GMP;
	else if (!memcmp(str, "NET", 3))
		return SubFile::NET;
	else
		return SubFile::Unknown;
}

IMG::IMG(const QString &fileName)
  : _file(fileName), _typ(0), _style(0), _valid(false)
{
#define CHECK(condition) \
	if (!(condition)) { \
		_errorString = "Unsupported or invalid IMG file"; \
		qDeleteAll(tileMap); \
		return; \
	}

	TileMap tileMap;
	QString typFile;

	if (!_file.open(QFile::ReadOnly)) {
		_errorString = _file.errorString();
		return;
	}

	// Read IMG header
	char signature[7], identifier[7];
	_file.read((char*)&_key, 1) && _file.seek(0x10)
	  && read(signature, sizeof(signature)) && _file.seek(0x41)
	  && read(identifier, sizeof(identifier));
	if (memcmp(signature, "DSKIMG", sizeof(signature))
	  || memcmp(identifier, "GARMIN", sizeof(identifier))) {
		_errorString = "Not a Garmin IMG file";
		return;
	}
	char d1[20], d2[31];
	quint8 e1, e2;
	CHECK(_file.seek(0x49) && read(d1, sizeof(d1)) && _file.seek(0x61)
	  && readValue(e1) && readValue(e2) && _file.seek(0x65)
	  && read(d2, sizeof(d2)));

	QByteArray nba(QByteArray(d1, sizeof(d1)) + QByteArray(d2, sizeof(d2)));
	_name = QString::fromLatin1(nba.constData(), nba.size()-1).trimmed();
	_blockSize = 1 << (e1 + e2);

	_polyCache.setMaxCost(CACHED_SUBDIVS_COUNT);
	_pointCache.setMaxCost(CACHED_SUBDIVS_COUNT);

	// Read the FAT table
	quint8 flag;
	quint64 offset = 0x200;
	// Skip unused FAT blocks if any
	while (true) {
		CHECK(_file.seek(offset) && readValue(flag));
		if (flag)
			break;
		offset += 512;
	}

	// Read first FAT block with FAT table size
	char name[8], type[3];
	quint32 size;
	quint16 part;
	CHECK(_file.seek(offset + 12) && readValue(size));
	offset += 512;
	int cnt = (size - offset) / 512;

	// Read FAT blocks describing the IMG sub-files
	for (int i = 0; i < cnt; i++) {
		quint16 block;
		CHECK(_file.seek(offset) && readValue(flag) && read(name, sizeof(name))
		  && read(type, sizeof(type)) && readValue(size) && readValue(part));
		SubFile::Type tt = tileType(type);

		QString fn(QByteArray(name, sizeof(name)));
		if (VectorTile::isTileFile(tt)) {
			VectorTile *tile;
			TileMap::const_iterator it = tileMap.find(fn);
			if (it == tileMap.constEnd()) {
				tile = new VectorTile();
				tileMap.insert(fn, tile);
			} else
				tile = *it;

			SubFile *file = part ? tile->file(tt)
			  : tile->addFile(this, tt);
			CHECK(file);

			_file.seek(offset + 0x20);
			for (int i = 0; i < 240; i++) {
				CHECK(readValue(block));
				if (block == 0xFFFF)
					break;
				file->addBlock(block);
			}
		} else if (tt == SubFile::TYP) {
			SubFile *typ = 0;
			if (typFile.isNull()) {
				_typ = new SubFile(this);
				typ = _typ;
				typFile = fn;
			} else if (fn == typFile)
				typ = _typ;

			if (typ) {
				_file.seek(offset + 0x20);
				for (int i = 0; i < 240; i++) {
					CHECK(readValue(block));
					if (block == 0xFFFF)
						break;
					typ->addBlock(block);
				}
			}
		}

		offset += 512;
	}

	// Create tile tree
	for (TileMap::const_iterator it = tileMap.constBegin();
	  it != tileMap.constEnd(); ++it) {
		VectorTile *tile = it.value();

		if (!tile->init()) {
			qWarning("%s: %s: Invalid map tile", qPrintable(_file.fileName()),
			  qPrintable(it.key()));
			delete tile;
			continue;
		}

		double min[2], max[2];
		min[0] = tile->bounds().left();
		min[1] = tile->bounds().bottom();
		max[0] = tile->bounds().right();
		max[1] = tile->bounds().top();
		_tileTree.Insert(min, max, tile);

		_bounds |= tile->bounds();
	}

	if (!_tileTree.Count())
		_errorString = "No usable map tile found";
	else
		_valid = true;
}

IMG::~IMG()
{
	TileTree::Iterator it;
	for (_tileTree.GetFirst(it); !_tileTree.IsNull(it); _tileTree.GetNext(it))
		delete _tileTree.GetAt(it);

	delete _typ;
	delete _style;
}

void IMG::load()
{
	Q_ASSERT(!_style);

	if (_typ)
		_style = new Style(_typ);
	else {
		QFile typFile(ProgramPaths::typFile());
		if (typFile.open(QIODevice::ReadOnly)) {
			SubFile typ(&typFile);
			_style = new Style(&typ);
		} else
			_style = new Style();
	}
}

void IMG::clear()
{
	TileTree::Iterator it;
	for (_tileTree.GetFirst(it); !_tileTree.IsNull(it); _tileTree.GetNext(it))
		_tileTree.GetAt(it)->clear();

	delete _style;
	_style = 0;

	_polyCache.clear();
	_pointCache.clear();
}

static bool polyCb(VectorTile *tile, void *context)
{
	PolyCTX *ctx = (PolyCTX*)context;
	tile->polys(ctx->rect, ctx->bits, ctx->polygons, ctx->lines, ctx->polyCache);
	return true;
}

static bool pointCb(VectorTile *tile, void *context)
{
	PointCTX *ctx = (PointCTX*)context;
	tile->points(ctx->rect, ctx->bits, ctx->points, ctx->pointCache);
	return true;
}

void IMG::polys(const RectC &rect, int bits, QList<Poly> *polygons,
  QList<Poly> *lines)
{
	PolyCTX ctx(rect, bits, polygons, lines, &_polyCache);
	double min[2], max[2];

	min[0] = rect.left();
	min[1] = rect.bottom();
	max[0] = rect.right();
	max[1] = rect.top();

	_tileTree.Search(min, max, polyCb, &ctx);
}

void IMG::points(const RectC &rect, int bits, QList<Point> *points)
{
	PointCTX ctx(rect, bits, points, &_pointCache);
	double min[2], max[2];

	min[0] = rect.left();
	min[1] = rect.bottom();
	max[0] = rect.right();
	max[1] = rect.top();

	_tileTree.Search(min, max, pointCb, &ctx);
}

qint64 IMG::read(char *data, qint64 maxSize)
{
	qint64 ret = _file.read(data, maxSize);
	if (_key)
		for (int i = 0; i < ret; i++)
			data[i] ^= _key;
	return ret;
}

template<class T> bool IMG::readValue(T &val)
{
	T data;

	if (read((char*)&data, sizeof(T)) < (qint64)sizeof(T))
		return false;

	val = qFromLittleEndian(data);

	return true;
}

bool IMG::readBlock(int blockNum, QByteArray &data)
{
	if (!_file.seek((qint64)blockNum * (qint64)_blockSize))
		return false;
	data.resize(_blockSize);
	if (read(data.data(), _blockSize) < _blockSize)
		return false;

	return true;
}

#ifndef QT_NO_DEBUG
QDebug operator<<(QDebug dbg, const IMG::Point &point)
{
	dbg.nospace() << "Point(" << hex << point.type << ", " << point.label
	  << ", " << point.poi << ")";
	return dbg.space();
}

QDebug operator<<(QDebug dbg, const IMG::Poly &poly)
{
	dbg.nospace() << "Poly(" << hex << poly.type << ", " << poly.label << ")";
	return dbg.space();
}
#endif // QT_NO_DEBUG
