#include "ConModule.h"
#include "Compat.h"
#include "stfs/STFSCommon.h"
#include <QFile>
namespace peare {
std::unique_ptr<ConModule> ConModule::open(const QString& filePath){ QFile f(filePath); if(!f.open(QIODevice::ReadOnly)){auto m=peare::makeUnique<ConModule>();m->info_.filePath=filePath;m->info_.format=ModuleFormat::CON;m->info_.error=f.errorString();return m;} return open(f.readAll(),filePath); }
std::unique_ptr<ConModule> ConModule::open(const QByteArray& data,const QString& logicalName){auto m=peare::makeUnique<ConModule>();m->info_.filePath=logicalName;m->info_.format=ModuleFormat::CON;const auto p=stfs::parse(data,QStringLiteral("CON "));if(!p.valid){m->info_.error=p.error;return m;}m->info_.description=p.description;stfs::populateResources(p,data,logicalName,ModuleFormat::CON,&m->resources_);return m;}
}
