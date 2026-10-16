// Godot Engine contributors. SPDX-License-Identifier: MIT
// Local protocol fixture only: never invokes Node, CLI, HDC, a broker or signing.
// Its HAP bytes are deliberately fake; marker values test diagnostic suppression.
#include <json/json.h>

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <limits.h>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
const char *secret_prefix = "AUTO_SIGN_FIXTURE_PRIVATE_";
const char *auth_message = "Run the local DevEco CLI login command, then retry Remote Deploy.";
const std::vector<std::string> public_keys = {
	"buildMode", "cliPath", "commandLineToolsRoot", "format", "hdcPath", "loginDataDir", "nodePath",
	"projectDir", "projectIdentity", "schemaVersion", "sdkRoot", "target", "teamId"
};

void require_that(bool p_value) {
	if (!p_value) {
		throw std::runtime_error("public fixture request mismatch");
	}
}

Json::Value parse(const std::string &p_text) {
	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;
	builder["allowComments"] = false;
	builder["allowTrailingCommas"] = false;
	builder["failIfExtra"] = true;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	Json::Value value;
	std::string errors;
	require_that(reader->parse(p_text.data(), p_text.data() + p_text.size(), &value, &errors));
	return value;
}

std::string json(const Json::Value &p_value) {
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	return Json::writeString(builder, p_value);
}

std::string read_file(const std::string &p_path) {
	std::ifstream stream(p_path, std::ios::binary);
	require_that(stream.is_open());
	std::ostringstream result;
	result << stream.rdbuf();
	require_that(!stream.bad());
	return result.str();
}

void write_file(const std::string &p_path, const std::string &p_bytes) {
	std::ofstream stream(p_path, std::ios::binary | std::ios::trunc);
	stream.exceptions(std::ios::badbit | std::ios::failbit);
	stream.write(p_bytes.data(), p_bytes.size());
	stream.close();
}

std::string parent(const std::string &p_path) {
	const size_t slash = p_path.rfind('/');
	require_that(slash != std::string::npos);
	return slash == 0 ? "/" : p_path.substr(0, slash);
}

bool is_path(const std::string &p_path, bool p_directory) {
	struct stat info = {};
	return !p_path.empty() && p_path[0] == '/' && stat(p_path.c_str(), &info) == 0 &&
			(p_directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode));
}

Json::Value configuration() {
	char executable[PATH_MAX];
	const ssize_t size = readlink("/proc/self/exe", executable, sizeof(executable));
	require_that(size > 0 && size < static_cast<ssize_t>(sizeof(executable)));
	return parse(read_file(parent(std::string(executable, size)) + "/auto-sign-fixture.json"));
}
} // namespace

int main(int argc, char **argv) {
	try {
		require_that(argc == 3 && std::string(argv[1]) == "--request-json");
		const std::string raw = argv[2];
		require_that(raw.find(secret_prefix) == std::string::npos);
		const Json::Value request = parse(raw);
		const Json::Value config = configuration();
		require_that(config.isObject() && config["schemaVersion"].isNumeric() && config["schemaVersion"].asDouble() == 1);
		require_that(config["case"].isString());
		const std::string name = config["case"].asString();
		const Json::Value expected = config["expected"];
		require_that(request.isObject() && expected.isObject());
		require_that(request.getMemberNames() == public_keys && expected.getMemberNames() == public_keys);
		require_that(request["schemaVersion"].isNumeric() && request["schemaVersion"].asDouble() == 1);
		require_that(expected["schemaVersion"].isNumeric() && expected["schemaVersion"].asDouble() == 1);
		for (const std::string &key : public_keys) {
			if (key != "schemaVersion") {
				require_that(request[key].isString() && expected[key].isString() && request[key] == expected[key]);
			}
		}
		require_that(is_path(request["projectIdentity"].asString(), false));
		const std::string project = request["projectDir"].asString();
		require_that(is_path(project, true));
		require_that(is_path(project + "/entry/src/main/resources/rawfile/template.pck", false));

		Json::Value audit(Json::objectValue);
		audit["schemaVersion"] = 1;
		audit["case"] = name;
		audit["backend"] = "native";
		audit["requestValidated"] = true;
		audit["requestSchemaVersion"] = request["schemaVersion"];
		audit["projectDir"] = project;
		audit["pid"] = Json::Int64(getpid());
		audit["request"] = request;
		write_file(project + "/fixture-audit.json", json(audit));
		const std::string hap = project + "/fake.hap";
		write_file(hap, "FAKE HAP protocol fixture: " + name + "\n");
		Json::Value result(Json::objectValue);
		result["schemaVersion"] = 1;
		result["status"] = "ok";
		result["signed"] = true;
		result["profileValidated"] = true;
		result["format"] = "hap";
		result["mode"] = "debug";
		result["outputPath"] = hap;
		int exit_code = 0;
		if (name == "ok") {
			// No confirmation step is needed for the successful public context.
		} else if (name == "auth_required") {
			result = Json::Value(Json::objectValue);
			result["schemaVersion"] = 1;
			result["status"] = "error";
			result["code"] = "AUTH_REQUIRED";
			result["message"] = auth_message;
			result["diagnostic"] = std::string(secret_prefix) + "backend_diagnostic";
			exit_code = 3;
		} else if (name == "confirmation_required") {
			result = Json::Value(Json::objectValue);
			result["schemaVersion"] = 1;
			result["status"] = "confirmation_required";
			result["confirmationToken"] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
			result["reason"] = "CACHE_MISSING";
			result["message"] = "Confirm creation of local fixture signing material in the Editor.";
		} else if (name == "invalid_json") {
			std::cout << "not-json " << secret_prefix << "malformed_stdout\n";
			return 0;
		} else if (name == "schema_number") {
			result["schemaVersion"] = 2;
		} else if (name == "schema_string") {
			result["schemaVersion"] = "1";
		} else if (name == "signed_false") {
			result["signed"] = false;
		} else if (name == "signed_string") {
			result["signed"] = "true";
		} else if (name == "profile_false") {
			result["profileValidated"] = false;
		} else if (name == "profile_string") {
			result["profileValidated"] = "true";
		} else if (name == "wrong_format") {
			result["format"] = "app";
		} else if (name == "wrong_mode") {
			result["mode"] = "release";
		} else if (name == "outside_project") {
			const std::string outside = parent(project) + "/outside-artifact.hap";
			result["outputPath"] = outside;
			write_file(outside, "FAKE outside-project HAP\n");
		} else if (name == "path_traversal") {
			require_that(mkdir((project + "/nested").c_str(), 0700) == 0);
			// Preserve literal /../ exactly as the JS fixture did.
			result["outputPath"] = project + "/nested/../fake.hap";
			require_that(is_path(result["outputPath"].asString(), false));
		} else if (name == "nonzero_exit") {
			exit_code = 23;
		} else if (name == "missing_artifact") {
			result["outputPath"] = project + "/absent.hap";
		} else {
			throw std::runtime_error("unknown public fixture case");
		}
		std::cout << json(result) << '\n';
		return exit_code;
	} catch (...) {
		// Never echo request contents, parser errors or private marker values.
		std::cout << "{\"schemaVersion\":1,\"status\":\"error\",\"code\":\"FIXTURE_REQUEST_INVALID\","
					 "\"message\":\"The local regression backend rejected the public fixture request.\"}\n";
		return 81;
	}
}
