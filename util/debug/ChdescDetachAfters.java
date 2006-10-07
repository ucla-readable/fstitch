public class ChdescDetachAfters extends Opcode
{
	private final int chdesc;
	
	public ChdescDetachAfters(int chdesc)
	{
		this.chdesc = chdesc;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public boolean hasEffect()
	{
		return false;
	}
	
	public String toString()
	{
		return "KDB_CHDESC_DETACH_AFTERS: chdesc = " + SystemState.hex(chdesc);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_DETACH_AFTERS, "KDB_CHDESC_DETACH_AFTERS", ChdescDetachAfters.class);
		factory.addParameter("chdesc", 4);
		return factory;
	}
}
