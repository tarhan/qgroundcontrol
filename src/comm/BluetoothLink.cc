/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include <QtGlobal>
#include <QTimer>
#include <QList>
#include <QDebug>
#include <iostream>

#include <QEventLoop>
#include <QtBluetooth/QBluetoothDeviceDiscoveryAgent>
#include <QtBluetooth/QBluetoothLocalDevice>
#include <QtBluetooth/QBluetoothUuid>
#include <QtBluetooth/QBluetoothSocket>
#include <QtBluetooth/QLowEnergyController>
#include <QtBluetooth/QLowEnergyService>

#include "QGCApplication.h"
#include "BluetoothLink.h"
#include "QGC.h"
#include "LinkManager.h"

BluetoothLink::BluetoothLink(SharedLinkConfigurationPtr& config)
    : LinkInterface     (config)
{

}

BluetoothLink::~BluetoothLink()
{
    disconnect();
#ifdef __ios__
    if(_discoveryAgent) {
        _shutDown = true;
        _discoveryAgent->stop();
        _discoveryAgent->deleteLater();
        _discoveryAgent = nullptr;
    }
#endif
}

void BluetoothLink::run()
{

}

void BluetoothLink::_writeBytes(const QByteArray bytes)
{
    if (_bleController) {
        bleWriteData(bytes);
        emit bytesSent(this, bytes);
    }
    if (_targetSocket) {
        if(_targetSocket->write(bytes) > 0) {
            emit bytesSent(this, bytes);
        } else {
            qWarning() << "Bluetooth write error";
        }
    }
}

void BluetoothLink::createBleController(const BluetoothData &device)
{
    if (_bleController) {
        _bleController->disconnectFromDevice();
        delete _bleController;
        _bleController = nullptr;
    }

    QBluetoothDeviceInfo info(QBluetoothAddress(device.address), device.name, 0);
    _bleController = QLowEnergyController::createCentral(info, this);

    connect(_bleController, &QLowEnergyController::serviceDiscovered,
            this, &BluetoothLink::bleServiceDiscovered);
    connect(_bleController, &QLowEnergyController::discoveryFinished,
            this, &BluetoothLink::bleServiceDiscoveryFinished);
    connect(_bleController, static_cast<void (QLowEnergyController::*)(QLowEnergyController::Error)>(&QLowEnergyController::error),
            this, &BluetoothLink::bleErrorOccured);
    connect(_bleController, &QLowEnergyController::stateChanged,
            this, &BluetoothLink::bleStateChanged);
    connect(_bleController, &QLowEnergyController::connected,
            this, &BluetoothLink::bleConnected);
    connect(_bleController, &QLowEnergyController::disconnected,
            this, &BluetoothLink::bleDisconnected);
    _bleController->connectToDevice();
}

void BluetoothLink::bleServiceDiscovered(const QBluetoothUuid &newService)
{
    qDebug() << "Service discovered: " << newService.toString();
    if (newService == QBluetoothUuid(QUuid(UARTSERVICEUUID))) {
        _isUartFound = true;
    }
}

void BluetoothLink::bleServiceDiscoveryFinished()
{
    qWarning() << "Device discovery finished";
    if (_bleService != nullptr) {
        delete _bleService;
        _bleService = nullptr;
    }

    if (_isUartFound) {
        qDebug() << "Connecting to UART service";
        _bleService = _bleController->createServiceObject(QBluetoothUuid(QUuid(UARTSERVICEUUID)), this);
    }

    if (_bleService == nullptr) {
        qDebug() << "UART service not found";
        return;
    }

    connect(_bleService, &QLowEnergyService::stateChanged,
            this, &BluetoothLink::bleServiceStateChanged);
    connect(_bleService, &QLowEnergyService::characteristicChanged,
            this, &BluetoothLink::bleReceiveData);
    connect(_bleService, &QLowEnergyService::descriptorWritten,
            this, &BluetoothLink::confirmedDescriptorWrite);
    _bleService->discoverDetails();
}

void BluetoothLink::bleErrorOccured(QLowEnergyController::Error error)
{
    _connectState = false;
    qWarning() << "Device error: " << error;
    emit communicationError(tr("Bluetooth Link Error"), "Device error: " + QString(error));
}

void BluetoothLink::bleStateChanged(QLowEnergyController::ControllerState state)
{
    qWarning() << "Device state changed to " << state;
}

void BluetoothLink::bleConnected()
{
    qWarning() << "Device connected";
    _bleController->discoverServices();
}

void BluetoothLink::bleDisconnected()
{
    if (_bleController) {
        _bleController->disconnectFromDevice();
        delete _bleController;
        _bleController = nullptr;
    }
    emit disconnected();
}

void BluetoothLink::bleWriteData(const QByteArray &data)
{
//    qDebug() << "Selecting TX characteristics";
    const QLowEnergyCharacteristic  RxChar = _bleService->characteristic(QBluetoothUuid(QUuid(RXTXUUID)));

    if (_connectState) {
//        qDebug() << "Before write characteristics";
        if (data.size() > CHUNK_SIZE) {
            int sentBytes = 0;
            while (sentBytes < data.size()) {
                _bleService->writeCharacteristic(RxChar, data.mid(sentBytes, CHUNK_SIZE), QLowEnergyService::WriteWithoutResponse);
//                bleWaitForWrite();
                sentBytes += CHUNK_SIZE;
                if (_bleService->error() != QLowEnergyService::NoError) {
                    qDebug() << "Error writing: " << _bleService->error();
                    return;
                }
            }

        } else {
            _bleService->writeCharacteristic(RxChar, data, QLowEnergyService::WriteWithoutResponse);
        }
//        qDebug() << "After write characteristics";
    }
}

void BluetoothLink::bleServiceStateChanged(QLowEnergyService::ServiceState newState)
{
    qWarning() << "Service state changed to " << newState;
    switch (newState) {
    case QLowEnergyService::ServiceDiscovered :
    {
        const QLowEnergyCharacteristic ch = _bleService->characteristic(QBluetoothUuid(QUuid(RXTXUUID)));
        if (!ch.isValid()) {
            qDebug() << "RX/TX characteristics not found";
            break;
        }

        // Enable notification
        qDebug() << "Enabling notifications";
        _notificationDescTx = ch.descriptor(QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
        if (_notificationDescTx.isValid()) {
            _bleService->writeDescriptor(_notificationDescTx, QByteArray::fromHex("0100"));
        }
        _connectState = true;
        emit connected();
    }
    default:
        break;
    }
}

void BluetoothLink::bleReceiveData(const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue)
{
    emit bytesReceived(this, newValue);
}

void BluetoothLink::confirmedDescriptorWrite(const QLowEnergyDescriptor &descriptor, const QByteArray &newValue)
{
    qDebug() << "Descriptor written. Id: " << descriptor.uuid().toString() << ". New value: " << newValue.toHex(' ');
    if (descriptor.isValid() && descriptor == _notificationDescTx) {
        if (newValue == QByteArray("0000")) {
            // disabled notifications -> assume disconnect intent
            _bleController->disconnectFromDevice();
            delete _bleService;
            _bleService = nullptr;
            delete _bleController;
            _bleController = nullptr;

            qDebug() << "Disconnected from device";
        }
    }
}

void BluetoothLink::bleWaitForWrite() {
    QEventLoop loop;
    connect(_bleService, SIGNAL(characteristicWritten(QLowEnergyCharacteristic,QByteArray)),
            &loop, SLOT(quit()));
    loop.exec();
}

void BluetoothLink::readBytes()
{
    if (_targetSocket) {
        while (_targetSocket->bytesAvailable() > 0) {
            QByteArray datagram;
            datagram.resize(_targetSocket->bytesAvailable());
            _targetSocket->read(datagram.data(), datagram.size());
            emit bytesReceived(this, datagram);
        }
    }
}

void BluetoothLink::disconnect(void)
{
#ifdef __ios__
    if(_discoveryAgent) {
        _shutDown = true;
        _discoveryAgent->stop();
        _discoveryAgent->deleteLater();
        _discoveryAgent = nullptr;
    }
#endif
    if(_targetSocket) {
        // This prevents stale signals from calling the link after it has been deleted
        QObject::disconnect(_targetSocket, &QBluetoothSocket::readyRead, this, &BluetoothLink::readBytes);
        _targetSocket->deleteLater();
        _targetSocket = nullptr;
        emit disconnected();
    }
    if (_bleController) {
        _bleController->disconnectFromDevice();
        delete _bleService;
        _bleService = nullptr;
        delete _bleController;
        _bleController = nullptr;
    }
    _connectState = false;
}

bool BluetoothLink::_connect(void)
{
    _hardwareConnect();
    return true;
}

bool BluetoothLink::_hardwareConnect()
{
#ifdef __ios__
    if(_discoveryAgent) {
        _shutDown = true;
        _discoveryAgent->stop();
        _discoveryAgent->deleteLater();
        _discoveryAgent = nullptr;
    }
    _discoveryAgent = new QBluetoothServiceDiscoveryAgent(this);
    QObject::connect(_discoveryAgent, &QBluetoothServiceDiscoveryAgent::serviceDiscovered, this, &BluetoothLink::serviceDiscovered);
    QObject::connect(_discoveryAgent, &QBluetoothServiceDiscoveryAgent::finished, this, &BluetoothLink::discoveryFinished);
    QObject::connect(_discoveryAgent, &QBluetoothServiceDiscoveryAgent::canceled, this, &BluetoothLink::discoveryFinished);
    _shutDown = false;
    _discoveryAgent->start();
#else
    const auto device = qobject_cast<BluetoothConfiguration*>(_config.get())->device();
    bool isBle = device.name.contains("(BLE)");
    qDebug() << "Bluetooth BLE? : " << device.isBle;
    qDebug() << "Bluetooth name: " << device.name;
    qDebug() << "Bluetooth UUID: " << device.address;
    if (isBle) {
        createBleController(device);
    } else {
        _createSocket();
        _targetSocket->connectToService(QBluetoothAddress(qobject_cast<BluetoothConfiguration*>(_config.get())->device().address), QBluetoothUuid(QBluetoothUuid::ServiceClassUuid::SerialPort));
    }
#endif
    return true;
}

void BluetoothLink::_createSocket()
{
    if(_targetSocket)
    {
        delete _targetSocket;
        _targetSocket = nullptr;
    }
    _targetSocket = new QBluetoothSocket(QBluetoothServiceInfo::RfcommProtocol, this);
    QObject::connect(_targetSocket, &QBluetoothSocket::connected, this, &BluetoothLink::deviceConnected);

    QObject::connect(_targetSocket, &QBluetoothSocket::readyRead, this, &BluetoothLink::readBytes);
    QObject::connect(_targetSocket, &QBluetoothSocket::disconnected, this, &BluetoothLink::deviceDisconnected);

    QObject::connect(_targetSocket, static_cast<void (QBluetoothSocket::*)(QBluetoothSocket::SocketError)>(&QBluetoothSocket::error),
            this, &BluetoothLink::deviceError);
}

#ifdef __ios__
void BluetoothLink::serviceDiscovered(const QBluetoothServiceInfo& info)
{
    if(!info.device().name().isEmpty() && !_targetSocket)
    {
        if(_config->device().uuid == info.device().deviceUuid() && _config->device().name == info.device().name())
        {
            _createSocket();
            _targetSocket->connectToService(info);
        }
    }
}
#endif

#ifdef __ios__
void BluetoothLink::discoveryFinished()
{
    if(_discoveryAgent && !_shutDown)
    {
        _shutDown = true;
        _discoveryAgent->deleteLater();
        _discoveryAgent = nullptr;
        if(!_targetSocket)
        {
            _connectState = false;
            emit communicationError("Could not locate Bluetooth device:", _config->device().name);
        }
    }
}
#endif

void BluetoothLink::deviceConnected()
{
    _connectState = true;
    emit connected();
}

void BluetoothLink::deviceDisconnected()
{
    _connectState = false;
    qWarning() << "Bluetooth disconnected";
}

void BluetoothLink::deviceError(QBluetoothSocket::SocketError error)
{
    _connectState = false;
    qWarning() << "Bluetooth error" << error;
    emit communicationError(tr("Bluetooth Link Error"), _targetSocket->errorString());
}

bool BluetoothLink::isConnected() const
{
    return _connectState;
}

//--------------------------------------------------------------------------
//-- BluetoothConfiguration

BluetoothConfiguration::BluetoothConfiguration(const QString& name)
    : LinkConfiguration(name)
    , _deviceDiscover(nullptr)
{

}

BluetoothConfiguration::BluetoothConfiguration(BluetoothConfiguration* source)
    : LinkConfiguration(source)
    , _deviceDiscover(nullptr)
    , _device(source->device())
{
}

BluetoothConfiguration::~BluetoothConfiguration()
{
    if(_deviceDiscover)
    {
        _deviceDiscover->stop();
        delete _deviceDiscover;
    }
}

QString BluetoothConfiguration::settingsTitle()
{
    if(qgcApp()->toolbox()->linkManager()->isBluetoothAvailable()) {
        return tr("Bluetooth Link Settings");
    } else {
        return tr("Bluetooth Not Available");
    }
}

void BluetoothConfiguration::copyFrom(LinkConfiguration *source)
{
    LinkConfiguration::copyFrom(source);
    auto* usource = qobject_cast<BluetoothConfiguration*>(source);
    Q_ASSERT(usource != nullptr);
    _device = usource->device();
}

void BluetoothConfiguration::saveSettings(QSettings& settings, const QString& root)
{
    settings.beginGroup(root);
    settings.setValue("deviceName", _device.name);
#ifdef __ios__
    settings.setValue("uuid", _device.uuid.toString());
#else
    settings.setValue("address",_device.address);
    settings.setValue("isBle", _device.isBle);
#endif
    settings.endGroup();
}

void BluetoothConfiguration::loadSettings(QSettings& settings, const QString& root)
{
    settings.beginGroup(root);
    _device.name    = settings.value("deviceName", _device.name).toString();
#ifdef __ios__
    QString suuid   = settings.value("uuid", _device.uuid.toString()).toString();
    _device.uuid    = QUuid(suuid);
#else
    _device.address = settings.value("address", _device.address).toString();
    _device.isBle = settings.value("isBle", _device.isBle).toBool();
#endif
    settings.endGroup();
}

void BluetoothConfiguration::stopScan()
{
    if(_deviceDiscover)
    {
        _deviceDiscover->stop();
        _deviceDiscover->deleteLater();
        _deviceDiscover = nullptr;
        emit scanningChanged();
    }
}

void BluetoothConfiguration::startScan()
{
    if(!_deviceDiscover) {
        _deviceDiscover = new QBluetoothDeviceDiscoveryAgent(this);
        connect(_deviceDiscover, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered,  this, &BluetoothConfiguration::deviceDiscovered);
        connect(_deviceDiscover, &QBluetoothDeviceDiscoveryAgent::finished,          this, &BluetoothConfiguration::doneScanning);
        emit scanningChanged();
    } else {
        _deviceDiscover->stop();
    }
    _nameList.clear();
    _deviceList.clear();
    emit nameListChanged();
    _deviceDiscover->setInquiryType(QBluetoothDeviceDiscoveryAgent::GeneralUnlimitedInquiry);
    _deviceDiscover->start();
}

void BluetoothConfiguration::deviceDiscovered(QBluetoothDeviceInfo info)
{
    if(!info.name().isEmpty() && info.isValid())
    {
#if 0
        qDebug() << "Name:           " << info.name();
        qDebug() << "Address:        " << info.address().toString();
        qDebug() << "Service Classes:" << info.serviceClasses();
        QList<QBluetoothUuid> uuids = info.serviceUuids();
        for (QBluetoothUuid uuid: uuids) {
            qDebug() << "Service UUID:   " << uuid.toString();
        }
#endif
        BluetoothData data;
        data.isBle = info.coreConfigurations() & QBluetoothDeviceInfo::LowEnergyCoreConfiguration;
        data.name    = info.name() + (data.isBle ? " (BLE)" : "");
#ifdef __ios__
        data.uuid    = info.deviceUuid();
#else
        data.address = info.address().toString();
#endif

        if(!_deviceList.contains(data))
        {
            _deviceList += data;
            _nameList   += data.name;
            emit nameListChanged();
            return;
        }
    }
}

void BluetoothConfiguration::doneScanning()
{
    if(_deviceDiscover)
    {
        _deviceDiscover->deleteLater();
        _deviceDiscover = nullptr;
        emit scanningChanged();
    }
}

void BluetoothConfiguration::setDevName(const QString &name)
{
    for(const BluetoothData& data: _deviceList)
    {
        if(data.name == name)
        {
            _device = data;
            emit devNameChanged();
#ifndef __ios__
            emit addressChanged();
#endif
            return;
        }
    }
}

QString BluetoothConfiguration::address()
{
#ifdef __ios__
    return {};
#else
    return _device.address;
#endif
}
