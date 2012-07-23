// vim: set sts=2 sw=2 et:
// encoding: utf-8
//
// Copyleft 2011 RIME Developers
// License: GPLv3
//
// 2011-12-10 GONG Chen <chen.sst@gmail.com>
//
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <rime_version.h>
#include <rime/common.h>
#include <rime/config.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/dict_compiler.h>
#include <rime/expl/customizer.h>
#include <rime/expl/deployment_tasks.h>
#include <rime/expl/user_dict_manager.h>

namespace fs = boost::filesystem;

namespace rime {

bool InstallationUpdate::Run(Deployer* deployer) {
  LOG(INFO) << "updating rime installation info.";
  fs::path shared_data_path(deployer->shared_data_dir);
  fs::path user_data_path(deployer->user_data_dir);
  fs::path installation_info(user_data_path / "installation.yaml");
  rime::Config config;
  std::string installation_id;
  std::string last_distro_code_name;
  std::string last_distro_version;
  std::string last_rime_version;
  if (config.LoadFromFile(installation_info.string())) {
    if (config.GetString("installation_id", &installation_id)) {
      LOG(INFO) << "installation info exists. installation id: "
                << installation_id;
      // for now:
      deployer->user_id = installation_id;
    }
    if (config.GetString("distribution_code_name", &last_distro_code_name)) {
      LOG(INFO) << "previous distribution: " << last_distro_code_name;
    }
    if (config.GetString("distribution_version", &last_distro_version)) {
      LOG(INFO) << "previous distribution version: " << last_distro_version;
    }
    if (config.GetString("rime_version", &last_rime_version)) {
      LOG(INFO) << "previous Rime version: " << last_rime_version;
    }
  }
  if (!installation_id.empty() &&
      last_distro_code_name == deployer->distribution_code_name &&
      last_distro_version == deployer->distribution_version &&
      last_rime_version == RIME_VERSION) {
    return true;
  }
  LOG(INFO) << "creating installation info.";
  time_t now = time(NULL);
  std::string time_str(ctime(&now));
  boost::trim(time_str);
  if (installation_id.empty()) {
    installation_id =
        boost::uuids::to_string(boost::uuids::random_generator()());
    LOG(INFO) << "generated installation id: " << installation_id;
    // for now:
    deployer->user_id = installation_id;
    config.SetString("installation_id", installation_id);
    config.SetString("install_time", time_str);
  }
  else {
    config.SetString("update_time", time_str);
  }
  if (!deployer->distribution_name.empty()) {
    config.SetString("distribution_name", deployer->distribution_name);
    LOG(INFO) << "distribution: " << deployer->distribution_name;
  }
  if (!deployer->distribution_code_name.empty()) {
    config.SetString("distribution_code_name",
                     deployer->distribution_code_name);
    LOG(INFO) << "distribution code name: "
              << deployer->distribution_code_name;
  }
  if (!deployer->distribution_version.empty()) {
    config.SetString("distribution_version",
                     deployer->distribution_version);
    LOG(INFO) << "distribution version: " << deployer->distribution_version;
  }
  config.SetString("rime_version", RIME_VERSION);
  LOG(INFO) << "Rime version: " << RIME_VERSION;
  return config.SaveToFile(installation_info.string());
}

bool WorkspaceUpdate::Run(Deployer* deployer) {
  LOG(INFO) << "updating workspace.";
  {
    scoped_ptr<DeploymentTask> t;
    t.reset(new ConfigFileUpdate("default.yaml", "config_version"));
    t->Run(deployer);
    t.reset(new SymlinkingPrebuiltDictionaries);
    t->Run(deployer);
  }

  fs::path shared_data_path(deployer->shared_data_dir);
  fs::path user_data_path(deployer->user_data_dir);
  fs::path default_config_path(user_data_path / "default.yaml");
  Config config;
  if (!config.LoadFromFile(default_config_path.string())) {
    LOG(ERROR) << "Error loading default config from '"
               << default_config_path.string() << "'.";
    return false;
  }
  ConfigListPtr schema_list = config.GetList("schema_list");
  if (!schema_list) {
    LOG(WARNING) << "schema list not defined.";
    return false;
  }

  LOG(INFO) << "updating schemas.";
  int success = 0;
  int failure = 0;
  ConfigList::Iterator it = schema_list->begin();
  for (; it != schema_list->end(); ++it) {
    ConfigMapPtr item = As<ConfigMap>(*it);
    if (!item) continue;
    ConfigValuePtr schema_property = item->GetValue("schema");
    if (!schema_property) continue;
    const std::string &schema_id(schema_property->str());
    fs::path schema_path = shared_data_path / (schema_id + ".schema.yaml");
    if (!fs::exists(schema_path)) {
      schema_path = user_data_path / (schema_id + ".schema.yaml");
    }
    scoped_ptr<DeploymentTask> t(new SchemaUpdate(schema_path.string()));
    if (t->Run(deployer))
      ++success;
    else
      ++failure;
  }
  LOG(INFO) << "finished updating schemas: "
            << success << " success, " << failure << " failure.";
  return failure == 0;
}

bool SchemaUpdate::Run(Deployer* deployer) {
  fs::path source_path(schema_file_);
  if (!fs::exists(source_path)) {
    LOG(ERROR) << "Error updating schema: nonexistent file '"
               << schema_file_ << "'.";
    return false;
  }
  Config config;
  std::string schema_id;
  if (!config.LoadFromFile(schema_file_) ||
      !config.GetString("schema/schema_id", &schema_id) ||
      schema_id.empty()) {
    LOG(ERROR) << "invalid schema definition in '" << schema_file_ << "'.";
    return false;
  }
  
  fs::path shared_data_path(deployer->shared_data_dir);
  fs::path user_data_path(deployer->user_data_dir);
  fs::path destination_path(user_data_path / (schema_id + ".schema.yaml"));
  Customizer customizer(source_path, destination_path, "schema/version");
  if (customizer.UpdateConfigFile()) {
    LOG(INFO) << "schema '" << schema_id << "' is updated.";
  }
  
  if (!config.LoadFromFile(destination_path.string())) {
    LOG(ERROR) << "Error loading schema file '"
               << destination_path.string() << "'.";
    return false;
  }
  std::string dict_name;
  if (!config.GetString("translator/dictionary", &dict_name)) {
    // not requiring a dictionary
    return true;
  }
  std::string dict_file_name(dict_name + ".dict.yaml");
  fs::path dict_path(source_path.parent_path() / dict_file_name);
  if (!fs::exists(dict_path)) {
    dict_path = shared_data_path / dict_file_name;
    if (!fs::exists(dict_path)) {
      LOG(ERROR) << "source file for dictionary '"
                 << dict_name << "' does not exist.";
      return false;
    }
  }
  DictionaryComponent component;
  scoped_ptr<Dictionary> dict;
  dict.reset(component.CreateDictionaryFromConfig(&config, "translator"));
  if (!dict) {
    LOG(ERROR) << "Error creating dictionary '" << dict_name << "'.";
    return false;
  }
  LOG(INFO) << "preparing dictionary '" << dict_name << "'.";
  DictCompiler dict_compiler(dict.get());
  dict_compiler.Compile(dict_path.string(), destination_path.string());
  LOG(INFO) << "dictionary '" << dict_name << "' is ready.";
  return true;
}

bool ConfigFileUpdate::Run(Deployer* deployer) {
  fs::path shared_data_path(deployer->shared_data_dir);
  fs::path user_data_path(deployer->user_data_dir);
  fs::path source_config_path(shared_data_path / file_name_);
  fs::path dest_config_path(user_data_path / file_name_);
  if (!fs::exists(source_config_path)) {
    LOG(WARNING) << "'" << file_name_
                 << "' is missing from shared data directory.";
    source_config_path = dest_config_path;
  }
  Customizer customizer(source_config_path, dest_config_path, version_key_);
  return customizer.UpdateConfigFile();
}

bool PrebuildAllSchemas::Run(Deployer* deployer) {
  fs::path shared_data_path(deployer->shared_data_dir);
  fs::path user_data_path(deployer->user_data_dir);
  if (!fs::exists(shared_data_path) || !fs::is_directory(shared_data_path))
    return false;
  fs::directory_iterator iter(shared_data_path);
  fs::directory_iterator end;
  bool success = true;
  for (; iter != end; ++iter) {
    fs::path entry(iter->path());
    if (boost::ends_with(entry.string(), ".schema.yaml")) {
      scoped_ptr<DeploymentTask> t(new SchemaUpdate(entry.string()));
      if (!t->Run(deployer))
        success = false;
    }
  }
  return success;
}

bool SymlinkingPrebuiltDictionaries::Run(Deployer* deployer) {
  fs::path shared_data_path(deployer->shared_data_dir);
  fs::path user_data_path(deployer->user_data_dir);
  if (!fs::exists(shared_data_path) || !fs::is_directory(shared_data_path) ||
      !fs::exists(user_data_path) || !fs::is_directory(user_data_path) ||
      fs::equivalent(shared_data_path, user_data_path))
    return false;
  bool success = false;
  // test existing link
  fs::directory_iterator test(user_data_path);
  fs::directory_iterator end;
  for (; test != end; ++test) {
    fs::path entry(test->path());
    if (fs::is_symlink(entry) && entry.extension().string() == ".bin") {
      try {
        if (!fs::exists(entry)) {
          LOG(INFO) << "removing dangling symlink: "
                    << entry.filename().string();
          fs::remove(entry);
        }
      }
      catch (const fs::filesystem_error& ex) {
        LOG(ERROR) << ex.what();
        success = false;
      }
    }
  }
  // create link
  fs::directory_iterator iter(shared_data_path);
  for (; iter != end; ++iter) {
    fs::path entry(iter->path());
    fs::path link(user_data_path / entry.filename());
    try {
      if (fs::is_regular_file(entry) &&
          entry.extension().string() == ".bin" &&
          !fs::exists(link)) {
        LOG(INFO) << "symlinking '" << entry.filename().string() << "'.";
        fs::create_symlink(entry, link);
      }
    }
    catch (const fs::filesystem_error& ex) {
      LOG(ERROR) << ex.what();
      success = false;
    }
  }
  return success;
}

bool UserDictUpgration::Run(Deployer* deployer) {
  UserDictManager manager(deployer);
  UserDictList dicts;
  manager.GetUserDictList(&dicts);
  bool ok = true;
  UserDictList::const_iterator it = dicts.begin();
  for (; it != dicts.end(); ++it) {
    if (!manager.UpgradeUserDict(*it))
      ok = false;
  }
  return ok;
}

}  // namespace rime
