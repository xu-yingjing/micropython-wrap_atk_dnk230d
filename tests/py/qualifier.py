import upywraptest

upywraptest.BuiltinValue( 0 )
upywraptest.BuiltinConstValue( 1 )
upywraptest.BuiltinConstReference( 'a' )
print( upywraptest.ReturnBuiltinValue( 'a' ) )
print( upywraptest.ReturnBuiltinConstReference( 'a' ) )

a = upywraptest.Q()
upywraptest.Value( a )
print( a.Get() )
upywraptest.Pointer( a )
print( a.Get() )
upywraptest.ConstPointer( a )
print( a.Get() )
print( 'shared' )
upywraptest.SharedPointer( a )
print( a.Get() )
upywraptest.ConstSharedPointer( a )
print( a.Get() )
upywraptest.ConstSharedPointerRef( a )
print( a.Get() )
print( 'reference' )
upywraptest.Reference( a )
print( a.Get() )
upywraptest.ConstReference( a )
print( a.Get() )
print( a.Address() == upywraptest.ReturnReference( a ).Address() )
print( upywraptest.ReturnSharedPointer().Get() )
