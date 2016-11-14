/*
 * SessionProjectTemplate.cpp
 *
 * Copyright (C) 2009-16 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <boost/function.hpp>
#include <boost/range/adaptors.hpp>

#include <core/Algorithm.hpp>
#include <core/Error.hpp>
#include <core/Exec.hpp>
#include <core/FilePath.hpp>
#include <core/FileSerializer.hpp>
#include <core/text/DcfParser.hpp>

#include <session/SessionModuleContext.hpp>

#include <session/SessionPackageProvidedExtension.hpp>

#define kProjectTemplateLocal "(local)"

using namespace rstudio::core;

namespace rstudio {
namespace session {
namespace modules {
namespace projects {
namespace templates {

namespace {

struct ProjectTemplateDescription
{
   std::string package;
   std::string binding;
   std::string title;
   std::string description;
   
   json::Value toJson() const
   {
      json::Object object;
      
      object["package"]     = package;
      object["binding"]     = binding;
      object["title"]       = title;
      object["description"] = description;
      
      return object;
   }
};

class ProjectTemplateRegistry
{
public:
   
   void add(const std::string& pkgName, const ProjectTemplateDescription& description)
   {
      registry_[pkgName].push_back(description);
   }
   
   std::vector<ProjectTemplateDescription>& get(const std::string& pkgName)
   {
      return registry_[pkgName];
   }
   
   std::size_t size()
   {
      return registry_.size();
   }
   
   json::Value toJson()
   {
      json::Object object;
      
      BOOST_FOREACH(const std::string& pkgName, registry_ | boost::adaptors::map_keys)
      {
         json::Array array;
         BOOST_FOREACH(const ProjectTemplateDescription& description, registry_[pkgName])
         {
            array.push_back(description.toJson());
         }
         object[pkgName] = array;
      }
      
      return object;
   }

   std::map<
      std::string,
      std::vector<ProjectTemplateDescription>
   > registry_;
};

ProjectTemplateRegistry& projectTemplateRegistry()
{
   static ProjectTemplateRegistry instance;
   return instance;
}

class ProjectTemplateIndexer : public ppe::Indexer
{
public:
   
   explicit ProjectTemplateIndexer(const std::string& resourcePath)
      : ppe::Indexer(resourcePath)
   {
   }
   
   void addIndexingFinishedCallback(boost::function<void()> callback)
   {
      callbacks_.push_back(callback);
   }
   
private:
   void onIndexingStarted()
   {
      pRegistry_ = boost::make_shared<ProjectTemplateRegistry>();
   }

   void onWork(const std::string& pkgName, const core::FilePath& resourcePath)
   {
      Error error;
      
      // read contents of resource file
      std::string contents;
      error = core::readStringFromFile(resourcePath, &contents, string_utils::LineEndingPosix);
      if (error)
         LOG_ERROR(error);
      
      // attempt to parse as dcf
      boost::regex reSeparator("\\n{2,}");
      boost::sregex_token_iterator it(contents.begin(), contents.end(), reSeparator, -1);
      boost::sregex_token_iterator end;
      
      for (; it != end; ++it)
      {
         // invoke parser on current field
         std::map<std::string, std::string> fields;
         std::string errorMessage;
         error = text::parseDcfFile(*it, true, &fields, &errorMessage);
         if (error)
         {
            LOG_ERROR(error);
            continue;
         }
         
         // validate fields of DCF file
         error = validateFields(fields, ERROR_LOCATION);
         if (error)
         {
            LOG_ERROR(error);
            continue;
         }
         
         // update registry
         ProjectTemplateDescription ptd;
         ptd.package     = pkgName;
         ptd.binding     = fields["binding"];
         ptd.title       = fields["title"];
         ptd.description = fields["description"];
         pRegistry_->add(pkgName, ptd);
      }
   }

   void onIndexingCompleted()
   {
      // index a project template file in the RStudio options folder
      FilePath localTemplatesPath =
            module_context::resolveAliasedPath("~/.R/rstudio/project_templates.dcf");
      
      if (localTemplatesPath.exists())
         onWork(kProjectTemplateLocal, localTemplatesPath);
      
      // update global registry
      projectTemplateRegistry() = *pRegistry_;
      
      // add known project templates
      addKnownProjectTemplates();
      
      // execute any callbacks waiting for indexing to complete
      executeCallbacks();
   }
   
private:
   Error validateFields(std::map<std::string, std::string>& fields,
                        const ErrorLocation& location)
   {
      std::vector<std::string> missingFields;
      const char* expected[] = {"binding", "title", "description"};
      for (std::size_t i = 0; i < sizeof(expected); ++i)
      {
         const char* field = expected[i];
         if (!fields.count(field) || fields[field].empty())
            missingFields.push_back(field);
      }
      
      if (missingFields.empty())
         return Success();
      
      std::string reason =
            "invalid project template description: missing or empty fields "
            "[" + core::algorithm::join(missingFields, ",") + "]";
      
      
      Error error = systemError(boost::system::errc::protocol_error, reason, location);
      return error;
   }
   
   void addKnownProjectTemplates()
   {
      addProjectTemplate(
               "devtools",
               "create",
               "Create an R Package using devtools",
               "Create a new R package following the devtools development conventions.");
      
      addProjectTemplate(
               "Rcpp",
               "Rcpp.package.skeleton",
               "Create an R Package using Rcpp",
               "Create a new R package using Rcpp.");
      
      addProjectTemplate(
               "RcppArmadillo",
               "RcppArmadillo.package.skeleton",
               "Create an R Package using RcppArmadillo",
               "Create a new R package using RcppArmadillo.");
      
      addProjectTemplate(
               "RcppEigen",
               "RcppEigen.package.skeleton",
               "Create an R Package using RcppEigen",
               "Create a new R package using RcppEigen.");
   }
   
   void addProjectTemplate(const std::string& package,
                           const std::string& binding,
                           const std::string& title,
                           const std::string& description)
   {
      // if we already have a project template registered for this
      // package with this binding, bail (this allows R packages to
      // override the default settings provided by RStudio if desired)
      std::vector<ProjectTemplateDescription>& templates =
            projectTemplateRegistry().get(package);
      
      for (std::size_t i = 0, n = templates.size(); i < n; ++i)
         if (templates[i].binding == binding)
            return;
      
      // add a new project template
      ProjectTemplateDescription ptd;
      ptd.package     = package;
      ptd.binding     = binding;
      ptd.title       = title;
      ptd.description = description;
      projectTemplateRegistry().add(package, ptd);
   }
   
   void executeCallbacks()
   {
      for (std::size_t i = 0, n = callbacks_.size(); i < n; ++i)
         callbacks_[i]();
      callbacks_.clear();
   }
   
private:
   std::vector< boost::function<void()> > callbacks_;
   boost::shared_ptr<ProjectTemplateRegistry> pRegistry_;
};

ProjectTemplateIndexer& projectTemplateIndexer()
{
   static ProjectTemplateIndexer instance("rstudio/project_templates.dcf");
   return instance;
}

void withProjectTemplateRegistry(boost::function<void()> callback)
{
   if (projectTemplateIndexer().running())
      projectTemplateIndexer().addIndexingFinishedCallback(callback);
   else
      callback();
}

void onDeferredInit(bool)
{
   if (module_context::disablePackages())
      return;
   
   projectTemplateIndexer().start();
}

void respondWithProjectTemplates(const json::JsonRpcFunctionContinuation& continuation)
{
   json::JsonRpcResponse response;
   response.setResult(projectTemplateRegistry().toJson());
   continuation(Success(), &response);
}

void getProjectTemplateRegistry(const json::JsonRpcRequest& request,
                                const json::JsonRpcFunctionContinuation& continuation)
{
   withProjectTemplateRegistry(
            boost::bind(respondWithProjectTemplates, boost::cref(continuation)));
}

} // end anonymous namespace

Error initialize()
{
   using namespace module_context;
   using boost::bind;
   
   events().onDeferredInit.connect(onDeferredInit);
   
   ExecBlock initBlock;
   initBlock.addFunctions()
         (bind(sourceModuleRFile, "SessionProjectTemplate.R"))
         (bind(registerAsyncRpcMethod, "get_project_template_registry", getProjectTemplateRegistry));
   return initBlock.execute();
}

} // end namespace templates
} // end namespace projects
} // end namespace modules
} // end namespace session
} // end namespace rstudio
