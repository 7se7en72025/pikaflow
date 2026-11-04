#include "pikaflow/frontend.hpp"
#include "pikaflow/ir.hpp"
#ifdef PF_HAS_LLVM
#include "pikaflow/backend.hpp"
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace fs=std::filesystem;
namespace {
std::string read(const fs::path& file) {
    std::ifstream input(file,std::ios::binary);if(!input)throw std::runtime_error("cannot open source file: "+file.u8string());
    input.seekg(0,std::ios::end);auto size=input.tellg();if(size<0 || size>16*1024*1024)throw std::runtime_error("source file exceeds 16 MiB limit");input.seekg(0);
    std::ostringstream content;content<<input.rdbuf();if(input.bad())throw std::runtime_error("cannot read source file: "+file.u8string());return content.str();
}
void write(const fs::path& file,const std::string& content) {
    std::ofstream output(file,std::ios::binary|std::ios::trunc);if(!output)throw std::runtime_error("cannot write output file: "+file.u8string());output<<content;if(!output)throw std::runtime_error("failed writing output file: "+file.u8string());
}
class Modules {
    std::set<std::string> active,loaded;
    std::vector<pika::StmtPtr> statements;
    void load(const fs::path& requested) {
        std::string path=fs::weakly_canonical(fs::absolute(requested)).u8string();
        if(active.count(path))throw pika::Error({path,1,1},"cyclic module import");if(loaded.count(path))return;active.insert(path);
        auto source=read(fs::u8path(path));auto tree=pika::parse(pika::lex(source,path),path);
        for(auto& statement:tree.statements)if(statement->kind==pika::Stmt::Kind::Import) {
            if(statement->other)throw pika::Error(statement->location,"named import aliases are not implemented; import exported names directly");
            auto imported=fs::weakly_canonical(fs::u8path(path).parent_path()/fs::u8path(statement->name));
            statement->name=imported.u8string();load(imported);
        }
        statements.insert(statements.end(),tree.statements.begin(),tree.statements.end());active.erase(path);loaded.insert(path);
    }
public:
    pika::SyntaxTree program(const fs::path& file) {
        std::string path=fs::weakly_canonical(fs::absolute(file)).u8string();load(file);return {std::move(statements),path};
    }
};
void diagnostic(const pika::Error& error) {
    std::cerr<<error.location.file<<":"<<error.location.line<<":"<<error.location.column<<": error: "<<error.what()<<"\n";
    try {
        std::istringstream input(read(fs::u8path(error.location.file)));std::string line;
        for(unsigned n=1;n<=error.location.line && std::getline(input,line);++n)if(n==error.location.line){std::cerr<<"  "<<line<<"\n  "<<std::string(error.location.column>0?error.location.column-1:0,' ')<<"^\n";}
    }catch(const std::exception&){}
}
void help() {
    std::cout<<"PikaFlow - typed structured and stack programming\n"
        <<"usage: pikaflow COMMAND FILE [-O0|-O1|-O2|-O3] [--debug] [-o OUTPUT]\n"
        <<"  check         parse, type-check and verify typed IR\n"
        <<"  interpret     execute the reference interpreter\n"
        <<"  run           execute through LLVM ORC JIT\n"
        <<"  emit-ir       print/write verified LLVM IR\n"
        <<"  emit-obj      emit a native object file (-o required)\n"
        <<"  build         compile and link a native executable\n"
        <<"  dump-tokens   inspect lexer output\n"
        <<"  dump-ast      inspect syntax tree\n"
        <<"  dump-ir       inspect custom typed IR\n"
        <<"  dump-asm      inspect target machine assembly\n"
        <<"  --version     show compiler/backend version\n";
}
}
int main(int argc,char** argv) {
    try {
        if(argc<2 || std::string(argv[1])=="--help" || std::string(argv[1])=="-h"){help();return 0;}
        std::string command=argv[1];
        if(command=="--version") {
            std::cout<<"PikaFlow 0.1.0; "
#ifdef PF_HAS_LLVM
                <<"LLVM enabled; "<<pika::targetTriple()
#else
                <<"LLVM disabled; reference interpreter only"
#endif
                <<"\n";return 0;
        }
        if(argc<3)throw std::runtime_error("missing source file; use --help");
        fs::path source=fs::u8path(argv[2]);std::string output;unsigned optimization=0;bool debug=false;
        for(int i=3;i<argc;++i){std::string option=argv[i];if(option=="-o"){if(++i>=argc)throw std::runtime_error("-o requires output path");output=argv[i];}else if(option=="--debug" || option=="-g")debug=true;else if(option.size()==3 && option[0]=='-' && option[1]=='O' && option[2]>='0' && option[2]<='3')optimization=static_cast<unsigned>(option[2]-'0');else throw std::runtime_error("unknown option: "+option);}
        std::string text;
        if(command=="dump-tokens"){text=pika::dumpTokens(pika::lex(read(source),fs::weakly_canonical(fs::absolute(source)).u8string()));}
        else {
            auto tree=Modules().program(source);
            if(command=="dump-ast")text=pika::dumpAST(tree);
            else {
                auto program=pika::compile(tree);
                if(command=="check"){std::cout<<"OK: "<<source.u8string()<<"\n";return 0;}
                if(command=="dump-ir")text=pika::dumpIR(program);
                else if(command=="interpret")return pika::interpret(program);
#ifdef PF_HAS_LLVM
                else if(command=="run")return pika::runJIT(program,optimization);
                else if(command=="emit-ir")text=pika::emitLLVM(program,optimization,debug);
                else if(command=="dump-asm")text=pika::emitAssembly(program,optimization,debug);
                else if(command=="emit-obj"){if(output.empty())throw std::runtime_error("emit-obj requires -o OUTPUT");pika::emitObject(program,output,optimization,debug);return 0;}
                else if(command=="build") {
                    if(output.empty()){output=source.stem().u8string();
#ifdef _WIN32
                        output+=".exe";
#endif
                    }
                    pika::buildExecutable(program,output,PF_RUNTIME_LIBRARY,PF_COMPILER_DRIVER,optimization,debug);std::cout<<"Built "<<output<<"\n";return 0;
                }
#else
                else if(command=="run" || command=="emit-ir" || command=="emit-obj" || command=="dump-asm" || command=="build")throw std::runtime_error("LLVM backend is disabled; configure with PIKAFLOW_ENABLE_LLVM=ON");
#endif
                else throw std::runtime_error("unknown command: "+command);
            }
        }
        if(output.empty())std::cout<<text;else write(fs::u8path(output),text);return 0;
    }catch(const pika::Error& error){diagnostic(error);return 1;}catch(const std::exception& error){std::cerr<<"pikaflow: "<<error.what()<<"\n";return 1;}
}
