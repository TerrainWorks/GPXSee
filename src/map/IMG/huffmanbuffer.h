#ifndef HUFFMANBUFFER_H
#define HUFFMANBUFFER_H

#include <QByteArray>
#include "subfile.h"

class RGNFile;

class HuffmanBuffer : public QByteArray
{
public:
	HuffmanBuffer(quint8 id) : _id(id) {}

	quint8 id() const {return _id;}
	bool load(const RGNFile *rgn, SubFile::Handle &rgnHdl);

private:
	quint8 _id;
};

#endif // HUFFMANBUFFER_H
