public class ChdescSetLength extends Opcode
{
	private final int chdesc;
	private final short length;
	
	public ChdescSetLength(int chdesc, short length)
	{
		this.chdesc = chdesc;
		this.length = length;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
			chdesc.setLength(length);
	}
	
	public String toString()
	{
		return "KDB_CHDESC_SET_LENGTH: chdesc = " + SystemState.hex(chdesc) + ", length = " + length;
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_SET_LENGTH, "KDB_CHDESC_SET_LENGTH", ChdescSetLength.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("length", 2);
		return factory;
	}
}
