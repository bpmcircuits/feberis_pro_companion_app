#pragma once
#define FBR_SCAN_METRIC_FIELDS 23
/* Indexes within the payload after FBR1/id/SCAN_METRICS. */
enum {
    MetricBoot,
    MetricSession,
    MetricRequestedGap,
    MetricDwell,
    MetricCaptureReady,
    MetricRadioMs,
    MetricIngestMs,
    MetricGapMs,
    MetricMaxGapMs,
    MetricPeriodMs,
    MetricIngestCpuUs,
    MetricCaptured,
    MetricDropped,
    MetricMalformed,
    MetricCacheOverflows,
    MetricMetadataHits,
    MetricMetadataMisses,
    MetricQueueHighWater,
    MetricBackpressureMs,
    MetricScanBufferBytes,
    MetricCaptureCacheBytes,
    MetricMainStackFree,
    MetricScanCapacity
};
extern const char* const fbr_scan_metric_names[FBR_SCAN_METRIC_FIELDS];
