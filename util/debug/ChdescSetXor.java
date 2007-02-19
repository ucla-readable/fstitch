public class ChdescSetXor extends Opcode
{
	private final int chdesc, xor;
	
	public ChdescSetXor(int chdesc, int xor)
	{
		this.chdesc = chdesc;
		this.xor = xor;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
			chdesc.setXor(xor);
	}
	
	public String toString()
	{
		return "KDB_CHDESC_SET_XOR: chdesc = " + SystemState.hex(chdesc) + ", xor = " + SystemState.hex(xor);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_SET_XOR, "KDB_CHDESC_SET_XOR", ChdescSetXor.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("xor", 4);
		return factory;
	}
}
