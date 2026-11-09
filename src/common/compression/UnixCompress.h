#pragma once

class QIODevice;

namespace peare {
namespace compression {

bool decompressUnixCompress(QIODevice* input, QIODevice* output);

}
}
