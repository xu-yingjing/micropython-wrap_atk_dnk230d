#ifndef MICROPYTHONMODULE_CLASSWRAPPER
#define MICROPYTHONMODULE_CLASSWRAPPER

//This flags enables the use of shared_ptr instead of raw pointers
//for the actual storage of all native types.
//Per default this is on since passing around raw pointers quickly
//leads to undefined behavior when garbage collection comes into play.
//For example, consider this, where X and Y are both created With ClassWrapper,
//and Y::Store() takes an X* or X& and keeps a pointer to it internally:
//
//  y = Y()
//  y.Store( X() )
//  gc.collect()
//
//The last line will get rid of the ClassWrapper instance for the X object
//(since gc can't find a corresponding py object anymore as that was not stored;
//with actual py objects this wouldn't happen), so now y has a pointer to a deleted X.
//The only proper way around is using shared_ptr instead: if ClassWrapper has a
//shared_ptr< X >, Store takes a shared_ptr< X > (which it should do after all
//if it's planning to keep the argument longer then the function scope) and
//we pass a copy of ClassWrapper's object to Store, all is fine: when deleting
//the garbage collected object, shared_ptr's destructor is called but the object
//is not deleted unless there are no references anymore.
#ifndef UPYWRAP_SHAREDPTROBJ
  #define UPYWRAP_SHAREDPTROBJ 1
#endif

#include "detail/index.h"
#include "detail/util.h"
#include "detail/functioncall.h"
#include "detail/callreturn.h"
#include <vector>
#include <cstdint>
#if UPYWRAP_SHAREDPTROBJ
  #include <memory>
#endif

namespace upywrap
{
  //Main logic for registering classes and their functions.
  //Usage:
  //
  //struct SomeClass
  //{
  //  SomeClass();
  //  int Foo( int );
  //  double Bar( const std::string& );
  //};
  //
  //struct Funcs
  //{
  //  func_name_def( Foo )
  //  func_name_def( Bar )
  //};
  //
  //ClassWrapper< SomeClass > wrap( "SomeClass", dict );
  //wrap.Def< Funcs::Foo >( &SomeClass::Foo );
  //wrap.Def< Funcs::Bar >( &SomeClass::Bar );
  //
  //This will register type "SomeClass" and given functions in dict,
  //so if dict is the global dict of a module "mod", the class
  //can be used in uPy like this:
  //
  //import mod;
  //x = mod.SomeClass();
  //x.Foo();
  //x.Bar();
  //
  //For supported arguments and return values see FromPyObj and ToPyObj classes.
  template< class T >
  class ClassWrapper
  {
  public:
#if UPYWRAP_SHAREDPTROBJ
    using native_obj_t = std::shared_ptr< T >;
#else
    using native_obj_t = T*;
#endif

    ClassWrapper( const char* name, mp_obj_module_t* mod ) :
      ClassWrapper( name, mod->globals )
    {
    }

    ClassWrapper( const char* name, mp_obj_dict_t* dict )
    {
      static bool init = false;
      if( !init )
      {
        OneTimeInit( name, dict );
        init = true;
      }
    }

    template< index_type name, class Ret, class... A >
    void Def( Ret( *f ) ( T*, A... ), typename SelectRetvalConverter< Ret >::type conv = nullptr )
    {
      DefImpl< name, Ret, decltype( f ), A... >( f, conv );
    }

    template< index_type name, class Ret, class... A >
    void Def( Ret( T::*f ) ( A... ), typename SelectRetvalConverter< Ret >::type conv = nullptr )
    {
      DefImpl< name, Ret, decltype( f ), A... >( f, conv );
    }

    template< index_type name, class Ret, class... A >
    void Def( Ret( T::*f ) ( A... ) const, typename SelectRetvalConverter< Ret >::type conv = nullptr )
    {
      DefImpl< name, Ret, decltype( f ), A... >( f, conv );
    }

    template< class A >
    void Setter( const char* name, void( *f )( T*, A ) )
    {
      SetterImpl< decltype( f ), A >( name, f );
    }

    template< class A >
    void Setter( const char* name, void( T::*f )( A ) )
    {
      SetterImpl< decltype( f ), A >( name, f );
    }

    template< class A >
    void Getter( const char* name, A( *f )( T* ) )
    {
      GetterImpl< decltype( f ), A >( name, f );
    }

    template< class A >
    void Getter( const char* name, A( T::*f )() const )
    {
      GetterImpl< decltype( f ), A >( name, f );
    }

    template< class A >
    void Property( const char* name, void( *fset )( T*, A ), A( *fget )( T* ) )
    {
      SetterImpl< decltype( fset ), A >( name, fset );
      GetterImpl< decltype( fget ), A >( name, fget );
    }

    template< class A >
    void Property( const char* name, void( T::*fset )( A ), A( T::*fget )() const )
    {
      SetterImpl< decltype( fset ), A >( name, fset );
      GetterImpl< decltype( fget ), A >( name, fget );
    }

    void DefInit()
    {
      DefInit<>();
    }

    template< class... A >
    void DefInit()
    {
      DefInit( ConstructorFactoryFunc< T, A... > );
    }

    template< class... A >
    void DefInit( T*( *f ) ( A... ) )
    {
      InitImpl< FixedFuncNames::Init, decltype( f ), A... >( f );
    }

    void DefExit( void( T::*f ) () )
    {
      ExitImpl< FixedFuncNames::Exit, decltype( f ) >( f );
    }

#if UPYWRAP_SHAREDPTROBJ
    static mp_obj_t AsPyObj( T* p, bool own )
    {
      if( own )
        return AsPyObj( native_obj_t( p ) );
      return AsPyObj( native_obj_t( p, NoDelete ) );
    }
#else
    static mp_obj_t AsPyObj( T* p, bool )
    {
      return AsPyObj( p );
    }
#endif

    static mp_obj_t AsPyObj( native_obj_t p )
    {
#ifdef UPYWRAP_NOFINALISER
      #define upywrap_new_obj m_new_obj
#else
      #define upywrap_new_obj m_new_obj_with_finaliser
#endif
      if( type.base.type == nullptr )
        RaiseTypeException( "Native type has not been registered" );
      auto o = upywrap_new_obj( this_type );
      o->base.type = &type;
      o->cookie = defCookie;
#if UPYWRAP_SHAREDPTROBJ
      new( &o->obj ) native_obj_t( std::move( p ) );
#else
      o->obj = p;
#endif
      return o;
    }

    static T* AsNativePtr( mp_obj_t arg )
    {
      auto native = (this_type*) arg;
      if( native->cookie != defCookie )
        RaiseTypeException( "Cannot convert this object to a native class instance" );
      return native->GetPtr();
    }

    static native_obj_t AsNativeObj( mp_obj_t arg )
    {
      auto native = (this_type*) arg;
      if( native->cookie != defCookie )
        RaiseTypeException( "Cannot convert this object to a native class instance" );
      return native->obj;
    }

  private:
    struct FixedFuncNames
    {
      func_name_def( Init )
      func_name_def( Exit )
      func_name_def( __del__ )
    };

    T* GetPtr()
    {
#if UPYWRAP_SHAREDPTROBJ
      return obj.get();
#else
      return obj;
#endif
    }

#if UPYWRAP_SHAREDPTROBJ
    static void NoDelete( T* )
    {
    }
#endif

    //native attribute store interface
    struct NativeSetterCallBase
    {
      virtual void Call( mp_obj_t self_in, mp_obj_t value ) = 0;
    };

    //native attribute load interface
    struct NativeGetterCallBase
    {
      virtual mp_obj_t Call( mp_obj_t self_in ) = 0;
    };

    template< class T >
    static typename T::mapped_type FindAttrChecked( T& map, qstr attr )
    {
      auto ret = map.find( attr );
      if( ret == map.end() )
        RaiseAttributeException( type.name, attr );
      return ret->second;
    }

    static bool store_attr( mp_obj_t self_in, qstr attr, mp_obj_t value )
    {
      this_type* self = (this_type*) self_in;
      FindAttrChecked( self->setters, attr )->Call( self, value );
      return true;
    }

    static void load_attr( mp_obj_t self_in, qstr attr, mp_obj_t* dest )
    {
      //uPy calls load_attr to find methods as well, so we have no choice but to go through them.
      //However if we find one, it's more performant than uPy's lookup (see mp_load_method_maybe)
      //because we know we have a proper map with only functions so we don't need x checks
      auto locals_map = &( (mp_obj_dict_t*) type.locals_dict )->map;
      auto elem = mp_map_lookup( locals_map, new_qstr( attr ), MP_MAP_LOOKUP );
      if( elem != nullptr )
      {
        //assert( mp_obj_is_callable( elem->value ) )
        dest[ 0 ] = elem->value;
        dest[ 1 ] = self_in;
      }
      else
      {
        this_type* self = (this_type*) self_in;
        *dest = FindAttrChecked( self->getters, attr )->Call( self );
      }
    }

    void OneTimeInit( std::string name, mp_obj_dict_t* dict )
    {
      const auto qname = qstr_from_str( name.data() );
      type.base.type = &mp_type_type;
      type.name = qname;
      type.locals_dict = (mp_obj_dict_t*) mp_obj_new_dict( 0 );
      type.make_new = nullptr;
      type.store_attr = store_attr;
      type.load_attr = load_attr;

      mp_obj_dict_store( dict, new_qstr( qname ), &type );
      //store our dict in the module's dict so it's reachable by the GC mark phase,
      //or in other words: prevent the GC from sweeping it!!
      mp_obj_dict_store( dict, new_qstr( ( name + "_locals" ).data() ), type.locals_dict );

      DelImpl();
    }

    void AddFunctionToTable( const qstr name, mp_obj_t fun )
    {
      mp_obj_dict_store( type.locals_dict, new_qstr( name ), fun );
    }

    void AddFunctionToTable( const char* name, mp_obj_t fun )
    {
      AddFunctionToTable( qstr_from_str( name ), fun );
    }

    template< index_type name, class Ret, class Fun, class... A >
    void DefImpl( Fun f, typename SelectRetvalConverter< Ret >::type conv )
    {
      typedef NativeMemberCall< name, Ret, A... > call_type;
      auto callerObject = call_type::CreateCaller( f );
      if( conv )
        callerObject->convert_retval = conv;
      functionPointers[ (void*) name ] = callerObject;
      auto call = sizeof...( A ) + 1 > UPYWRAP_MAX_NATIVE_ARGS ? (void*) call_type::CallN : (void*) call_type::Call;
      AddFunctionToTable( name(), mp_make_function_n( 1 + sizeof...( A ), call ) );
    }

    template< class Fun, class A >
    void SetterImpl( const char* name, Fun f )
    {
      setters[ qstr_from_str( name ) ] = new NativeSetterCall< A >( f );
    }

    template< class Fun, class A >
    void GetterImpl( const char* name, Fun f )
    {
      getters[ qstr_from_str( name ) ] = new NativeGetterCall< A >( f );
    }

    void DelImpl()
    {
      typedef NativeMemberCall< FixedFuncNames::__del__, void, T* > call_type;
      auto call = (void*) call_type::Delete;
      AddFunctionToTable( FixedFuncNames::__del__(), mp_make_function_n( 1, call ) );
    }

    template< index_type name, class Fun, class... A >
    void InitImpl( Fun f )
    {
      typedef NativeMemberCall< name, T*, A... > call_type;
      functionPointers[ (void*) name ] = call_type::CreateCaller( f );
      type.make_new = call_type::MakeNew;
    }

    template< index_type name, class Fun >
    void ExitImpl( Fun f )
    {
      typedef NativeMemberCall< name, void > call_type;
      functionPointers[ (void*) name ] = call_type::CreateCaller( f );
      AddFunctionToTable( MP_QSTR___enter__, (mp_obj_t) &mp_identity_obj );
      AddFunctionToTable( MP_QSTR___exit__, mp_make_function_n( 4, (void*) call_type::CallDiscard ) );
    }

    //wrap native setter in function with uPy store_attr compatible signature
    template< class A >
    struct NativeSetterCall : NativeSetterCallBase
    {
      typedef InstanceFunctionCall< T, void, A > call_type;
    
      NativeSetterCall( typename call_type::func_type f ) :
        f( new NonMemberFunctionCall< T, void, A >( f ) )
      {
      }

      NativeSetterCall( typename call_type::mem_func_type f ) :
        f( new MemberFunctionCall< T, void, A >( f ) )
      {
      }

      void Call( mp_obj_t self_in, mp_obj_t value )
      {
        auto self = (this_type*) self_in;
        CallReturn< void, A >::Call( f, self->GetPtr(), value );
      }

    private:
      call_type* f;
    };

    //wrap native getter in function with uPy load_attr compatible signature
    template< class A >
    struct NativeGetterCall : NativeGetterCallBase
    {
      typedef InstanceFunctionCall< T, A > call_type;
    
      NativeGetterCall( typename call_type::func_type f ) :
        f( new NonMemberFunctionCall< T, A >( f ) )
      {
      }

      NativeGetterCall( typename call_type::const_mem_func_type f ) :
        f( new ConstMemberFunctionCall< T, A >( f ) )
      {
      }

      mp_obj_t Call( mp_obj_t self_in )
      {
        auto self = (this_type*) self_in;
        return CallReturn< A >::Call( f, self->GetPtr() );
      }

    private:
      call_type* f;
    };

    //wrap native call in function with uPy compatible mp_obj_t( mp_obj_t self, mp_obj_t.... ) signature
    template< index_type index, class Ret, class... A >
    struct NativeMemberCall
    {
      typedef InstanceFunctionCall< T, Ret, A... > call_type;
      typedef FunctionCall< T*, A... > init_call_type;
      typedef typename call_type::func_type func_type;
      typedef typename call_type::mem_func_type mem_func_type;
      typedef typename call_type::const_mem_func_type const_mem_func_type;
      typedef typename init_call_type::func_type init_func_type;

      static call_type* CreateCaller( func_type f )
      {
        return new NonMemberFunctionCall< T, Ret, A... >( f );
      }

      static call_type* CreateCaller( mem_func_type f )
      {
        return new MemberFunctionCall< T, Ret, A... >( f );
      }

      static call_type* CreateCaller( const_mem_func_type f )
      {
        return new ConstMemberFunctionCall< T, Ret, A... >( f );
      }

      static init_call_type* CreateCaller( init_func_type f )
      {
        return new init_call_type( f );
      }

      static mp_obj_t Call( mp_obj_t self_in, typename project2nd< A, mp_obj_t >::type... args )
      {
        auto self = (this_type*) self_in;
        auto f = (call_type*) this_type::functionPointers[ (void*) index ];
        return CallReturn< Ret, A... >::Call( f, self->GetPtr(), args... );
      }

      static mp_obj_t CallN( uint n_args, const mp_obj_t* args )
      {
        if( n_args != sizeof...( A ) + 1 )
          RaiseTypeException( "Wrong number of arguments" );
        auto self = (this_type*) args[ 0 ];
        auto firstArg = &args[ 1 ];
        auto f = (call_type*) this_type::functionPointers[ (void*) index ];
        return callvar( f, self->GetPtr(), firstArg, make_index_sequence< sizeof...( A ) >() );
      }

      static mp_obj_t CallDiscard( uint n_args, const mp_obj_t* args )
      {
        (void) n_args;
        static_assert( sizeof...( A ) == 0, "Arguments must be discarded" );
        auto self = (this_type*) args[ 0 ];
        auto f = (call_type*) this_type::functionPointers[ (void*) index ];
        return CallReturn< Ret, A... >::Call( f, self->GetPtr() );
      }

      static mp_obj_t MakeNew( mp_obj_t, uint n_args, uint, const mp_obj_t *args )
      {
        if( n_args != sizeof...( A ) )
          RaiseTypeException( "Wrong number of arguments for constructor" );
        auto f = (init_call_type*) this_type::functionPointers[ (void*) index ];
        UPYWRAP_TRY
        return AsPyObj( apply( f, args, make_index_sequence< sizeof...( A ) >() ), true );
        UPYWRAP_CATCH
      }

      static mp_obj_t Delete( mp_obj_t self_in )
      {
        auto self = (this_type*) self_in;
#if UPYWRAP_SHAREDPTROBJ
        self->obj.~shared_ptr< T >();
#else
        delete self->obj;
#endif
        return ToPyObj< void >::Convert();
      }

    private:
      template< size_t... Indices >
      static T* apply( init_call_type* f, const mp_obj_t* args, index_sequence< Indices... > )
      {
        (void) args;
        return f->Call( SelectFromPyObj< A >::type::Convert( args[ Indices ] )... );
      }

      template< size_t... Indices >
      static mp_obj_t callvar( call_type* f, T* self, const mp_obj_t* args, index_sequence< Indices... > )
      {
        (void) args;
        return CallReturn< Ret, A... >::Call( f, self, args[ Indices ]... );
      }
    };

    typedef ClassWrapper< T > this_type;
    using store_attr_map = std::map< qstr, NativeSetterCallBase* >;
    using load_attr_map = std::map< qstr, NativeGetterCallBase* >;

    mp_obj_base_t base; //must always be the first member!
    std::int64_t cookie; //we'll use this to check if a pointer really points to a ClassWrapper
    native_obj_t obj;
    static mp_obj_type_t type;
    static function_ptrs functionPointers;
    static store_attr_map setters;
    static load_attr_map getters;
    static const std::int64_t defCookie;
  };

  template< class T >
  mp_obj_type_t ClassWrapper< T >::type = { nullptr };

  template< class T >
  function_ptrs ClassWrapper< T >::functionPointers;

  template< class T >
  typename ClassWrapper< T >::store_attr_map ClassWrapper< T >::setters;

  template< class T >
  typename ClassWrapper< T >::load_attr_map ClassWrapper< T >::getters;

  template< class T >
  const std::int64_t ClassWrapper< T >::defCookie = 0x12345678908765;


  //Get instance pointer out of mp_obj_t
  template< class T >
  struct ClassFromPyObj< T* >
  {
    static T* Convert( mp_obj_t arg )
    {
      return ClassWrapper< T >::AsNativePtr( arg );
    }
  };

#if UPYWRAP_SHAREDPTROBJ
  template< class T >
  struct ClassFromPyObj< std::shared_ptr< T > >
  {
    static std::shared_ptr< T > Convert( mp_obj_t arg )
    {
      return ClassWrapper< T >::AsNativeObj( arg );
    }
  };
#endif

  template< class T >
  struct ClassFromPyObj< T& >
  {
    static T& Convert( mp_obj_t arg )
    {
      return *ClassFromPyObj< T* >::Convert( arg );
    }
  };

  template< class T >
  struct ClassFromPyObj
  {
    static T Convert( mp_obj_t arg )
    {
      return *ClassFromPyObj< T* >::Convert( arg );
    }
  };

  //Wrap instance in a new mp_obj_t
  template< class T >
  struct ClassToPyObj< T* >
  {
    static mp_obj_t Convert( T* p )
    {
      return ClassWrapper< T >::AsPyObj( p, false );
    }
  };

#if UPYWRAP_SHAREDPTROBJ
  template< class T >
  struct ClassToPyObj< std::shared_ptr< T > >
  {
    static mp_obj_t Convert( std::shared_ptr< T > p )
    {
      return ClassWrapper< T >::AsPyObj( p );
    }
  };
#endif

  template< class T >
  struct ClassToPyObj< T& >
  {
    static mp_obj_t Convert( T& p )
    {
      return ClassToPyObj< T* >::Convert( &p );
    }
  };
}

#endif
