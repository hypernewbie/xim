extern "C" int xim_c_call_cpp(void);

extern "C" int
xim_cpp_increment(int value)
{
    return value + 1;
}

int
main()
{
    return xim_c_call_cpp() == 42 ? 0 : 1;
}
