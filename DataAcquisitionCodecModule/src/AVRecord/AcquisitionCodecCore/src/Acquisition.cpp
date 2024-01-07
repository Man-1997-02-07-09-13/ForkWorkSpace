#include "./src/FFmpegHeader.h"
#include "./common/timer.h"
#include "./src/Acquisition.h"
#include "RecordConfig.h"
#include "VideoCapture.h"
#include "VideoFrameQueue.h"
#include "AudioCapture.h"
#include "AudioFrameQueue.h"
#include "FileOutputer.h"
#include "FFmpegHelper.h"
#include "./common/util/util.h"

#include <QDateTime>
#include <QDebug>
#include <QApplication>
#include <QDesktopWidget>

using namespace std;
using namespace std::placeholders;
using namespace std::chrono;

ACC_API std::unique_ptr<AcquisitionCodec> createAcquisition(const QVariantMap& recordInfo)
{
    return make_unique<Acquisition>(recordInfo);
}

Acquisition::Acquisition(const QVariantMap& recordInfo)
{

    m_pauseStopwatch = make_unique<Timer<std::chrono::system_clock>>();
	setRecordInfo(recordInfo);
	m_videoCap = new VideoCapture;
	m_videoCap->setFrameCb(bind(&Acquisition::writeVideoFrameCb, this, _1, _2));
    m_videoFrameQueue = new VideoFrameQueue;
    if (g_record.enableAudio)
    {
        m_audioCap = new AudioCapture;
        m_audioCap->setFrameCb(bind(&Acquisition::writeAudioFrameCb, this, _1, _2));
        m_audioFrameQueue = new AudioFrameQueue;
    }

	m_outputer = new FileOutputer;
	m_outputer->setVideoFrameCb(bind(&Acquisition::readVideoFrameCb, this));
    if (g_record.enableAudio)
    {
        m_outputer->setAudioBufCb(bind(&Acquisition::initAudioBufCb, this, _1));
        m_outputer->setAudioFrameCb(bind(&Acquisition::readAudioFrameCb, this));
        m_outputer->setPauseDurationCb(bind(&Acquisition::getPauseDurationCb, this));
    }
}

Acquisition::~Acquisition()
{
    if (m_videoCap)
    {
		delete m_videoCap;
		m_videoCap = nullptr;
	}
    if (m_videoFrameQueue)
    {
		delete m_videoFrameQueue;
		m_videoFrameQueue = nullptr;
	}
    if (m_audioCap)
    {
        delete m_audioCap;
        m_audioCap = nullptr;
    }
    if (m_audioFrameQueue)
    {
        delete m_audioFrameQueue;
        m_audioFrameQueue = nullptr;
    }
    if (m_outputer)
    {
		delete m_outputer;
		m_outputer = nullptr;
	}
}

void Acquisition::setRecordInfo(const QVariantMap& recordInfo)
{
    g_record.filePath         = recordInfo["recordPath"].toString();
    g_record.inWidth          = util::screenWidth();
    g_record.inHeight         = util::screenHeight();
    g_record.outWidth         = recordInfo["outWidth"].toInt();
    g_record.outHeight        = recordInfo["outHeight"].toInt();
    g_record.fps              = recordInfo["fps"].toInt();
    g_record.enableAudio      = recordInfo["enableAudio"].toBool();
    g_record.audioDeviceIndex = recordInfo["audioDeviceIndex"].toInt();
    g_record.channel          = recordInfo["channel"].toInt();

    qInfo() << QString("Record info filePath:%1,inWidth:%2,inHeight:%3,outWidth:%4,outHeight:%5,fps:%6,enableAudio:%7,audioDeviceIndex:%8,channel:%9")
                   .arg(g_record.filePath)
                   .arg(g_record.inWidth)
                   .arg(g_record.inHeight)
                   .arg(g_record.outWidth)
                   .arg(g_record.outHeight)
                   .arg(g_record.fps)
                   .arg(g_record.enableAudio)
                   .arg(g_record.audioDeviceIndex)
                   .arg(g_record.channel);
}

/**
 * ���̣߳�
 * ��ʼ��FIFO buf
 * �򿪱�����
 * ��ʼ��������
 * �Ӹ�ʽ�����Ĵ�����stream���ӱ����������Ŀ�����������
 * 
 * �ɼ��̣߳��ɼ������ţ�д��FIFO
 * 
 * ���븴���̣߳�
 * ��FIFO��һ֡frame
 * �����packet
 * ��packetд���ļ�
 * 
 * @return 
*/
int Acquisition::startRecord(bool b)
{
	if (Running == g_record.status) return -1;

	FFmpegHelper::registerAll();
	startCapture(b);
	// init
    m_videoFrameQueue->initBuf(g_record.outWidth, g_record.outHeight, AV_PIX_FMT_YUV420P);
	m_outputer->init();
	// start
    m_startTime = duration_cast<chrono::microseconds>(chrono::system_clock::now().time_since_epoch()).count();
    qInfo() << "start time:" << QDateTime::fromMSecsSinceEpoch(m_startTime / 1000).toString("yyyy-MM-dd hh:mm:ss.zzz");
    m_outputer->start(m_startTime);
	g_record.status = Running;

	return 0;
}

int Acquisition::pauseRecord()
{
    if (Running != g_record.status) return -1;
    g_record.status = Paused;
    m_pauseStopwatch->start();
	return 0;
}

int Acquisition::resumeRecord()
{
    if (Paused != g_record.status) return -1;
    g_record.status = Running;
    g_record.cvNotPause.notify_all();
    m_pauseDuration += m_pauseStopwatch->delta<Timer<>::us>();

    return 0;
}

int Acquisition::stopRecord()
{
    RecordStatus oldStatus = g_record.status;
    if (Stopped == oldStatus) return -1;
    g_record.status = Stopped;
    if (Paused == oldStatus)
    {
        g_record.cvNotPause.notify_all();
    }

	stopCapture();
	m_outputer->stop();
	m_outputer->deinit();
	m_videoFrameQueue->deinit();
    if (g_record.enableAudio)
    {
        m_audioFrameQueue->deinit();
    }
	g_record.status = Stopped;

	return 0;
}

void Acquisition::startCapture(bool b)
{
    if (b)
	    m_videoCap->startCapture();
    if (g_record.enableAudio)
    {
        int ret = m_audioCap->startCapture();
		// �Ҳ�����Ƶ���ʧ��
        if (-1 == ret)
        {
            g_record.enableAudio = false;
		}
    }
}

void Acquisition::stopCapture()
{
	m_videoCap->stopCapture();
    if (g_record.enableAudio)
    {
        m_audioCap->stopCapture();
    }
}

void Acquisition::writeVideoFrameCb(AVFrame* frame, const VideoCaptureInfo& info)
{
    if (Running == g_record.status)
    {
        int64_t now         = duration_cast<chrono::microseconds>(chrono::system_clock::now().time_since_epoch()).count();
        int64_t captureTime = now - m_startTime - m_pauseDuration; // pts = ��ǰʱ��� - ��ʼʱ��� - ��ͣ��ʱ��
        m_videoFrameQueue->writeFrame(frame, info, captureTime);
	}
}

FrameItem* Acquisition::readVideoFrameCb()
{
	return m_videoFrameQueue->readFrame();
}

void Acquisition::initAudioBufCb(AVCodecContext* encodeCtx)
{
    if (m_audioFrameQueue)
    {
        m_audioFrameQueue->initBuf(encodeCtx);
    }
}

void Acquisition::writeAudioFrameCb(AVFrame* frame, const AudioCaptureInfo& info) {
    if (Running == g_record.status)
    {
        if (m_audioFrameQueue)
        {
            m_audioFrameQueue->writeFrame(frame, info);
        }
    }
}

AVFrame* Acquisition::readAudioFrameCb()
{
    if (m_audioFrameQueue)
    {
        return m_audioFrameQueue->readFrame();
    }
    return nullptr;
}