// Copyright (c) 2017 Rockset.
#ifndef ROCKSDB_LITE

#include "cloud/manifest_reader.h"
#include "cloud/aws/aws_env.h"
#include "cloud/cloud_manifest.h"
#include "cloud/db_cloud_impl.h"
#include "cloud/filename.h"
#include "db/version_set.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/options.h"
#include "rocksdb/status.h"
#include "util/file_reader_writer.h"

namespace rocksdb {

ManifestReader::ManifestReader(std::shared_ptr<Logger> info_log, CloudEnv* cenv,
                               const std::string& bucket_prefix)
    : info_log_(info_log), cenv_(cenv), bucket_prefix_(bucket_prefix) {}

ManifestReader::~ManifestReader() {}

//
// Extract all the live files needed by this MANIFEST file
//
Status ManifestReader::GetLiveFiles(const std::string bucket_path,
                                    std::set<uint64_t>* list) {
  Status s;
  unique_ptr<CloudManifest> cloud_manifest;
  {
    unique_ptr<SequentialFile> file;
    s = cenv_->NewSequentialFileCloud(
        bucket_prefix_, CloudManifestFile(bucket_path), &file, EnvOptions());
    if (!s.ok()) {
      return s;
    }
    s = CloudManifest::LoadFromLog(
        unique_ptr<SequentialFileReader>(
            new SequentialFileReader(std::move(file))),
        &cloud_manifest);
    if (!s.ok()) {
      return s;
    }
  }
  unique_ptr<SequentialFileReader> file_reader;
  {
    unique_ptr<SequentialFile> file;
    s = cenv_->NewSequentialFileCloud(
        bucket_prefix_,
        ManifestFileWithEpoch(bucket_path,
                              cloud_manifest->GetCurrentEpoch().ToString()),
        &file, EnvOptions());
    if (!s.ok()) {
      return s;
    }
    file_reader.reset(new SequentialFileReader(std::move(file)));
  }

  // create a callback that gets invoked whil looping through the log records
  VersionSet::LogReporter reporter;
  reporter.status = &s;
  log::Reader reader(nullptr, std::move(file_reader), &reporter,
                     true /*checksum*/, 0 /*initial_offset*/, 0);

  Slice record;
  std::string scratch;
  int count = 0;

  while (reader.ReadRecord(&record, &scratch) && s.ok()) {
    VersionEdit edit;
    s = edit.DecodeFrom(record);
    if (!s.ok()) {
      break;
    }
    count++;

    // add the files that are added by this transaction
    std::vector<std::pair<int, FileMetaData>> new_files = edit.GetNewFiles();
    for (auto& one : new_files) {
      uint64_t num = one.second.fd.GetNumber();
      list->insert(num);
    }
    // delete the files that are removed by this transaction
    std::set<std::pair<int, uint64_t>> deleted_files = edit.GetDeletedFiles();
    for (auto& one : deleted_files) {
      uint64_t num = one.second;
      list->erase(num);
    }
  }
  file_reader.reset();
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[mn] manifest for db %s has %d entries %s", bucket_path.c_str(), count,
      s.ToString().c_str());
  return s;
}

Status ManifestReader::GetMaxFileNumberFromManifest(Env* env,
                                                    const std::string& fname,
                                                    uint64_t* maxFileNumber) {
  // We check if the file exists to return IsNotFound() error status if it does
  // (NewSequentialFile) doesn't have the same behavior on file not existing --
  // it returns IOError instead.
  auto s = env->FileExists(fname);
  if (!s.ok()) {
    return s;
  }
  unique_ptr<SequentialFile> file;
  s = env->NewSequentialFile(fname, &file, EnvOptions());
  if (!s.ok()) {
    return s;
  }

  VersionSet::LogReporter reporter;
  reporter.status = &s;
  log::Reader reader(NULL, unique_ptr<SequentialFileReader>(
                               new SequentialFileReader(std::move(file))),
                     &reporter, true /*checksum*/, 0 /*initial_offset*/, 0);

  Slice record;
  std::string scratch;

  *maxFileNumber = 0;
  while (reader.ReadRecord(&record, &scratch) && s.ok()) {
    VersionEdit edit;
    s = edit.DecodeFrom(record);
    if (!s.ok()) {
      break;
    }
    uint64_t f;
    if (edit.GetNextFileNumber(&f)) {
      assert(*maxFileNumber <= f);
      *maxFileNumber = f;
    }
  }
  return s;
}
}
#endif /* ROCKSDB_LITE */
