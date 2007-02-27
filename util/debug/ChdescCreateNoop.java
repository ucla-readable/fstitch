public class ChdescCreateNoop extends Opcode
{
	private final int chdesc, owner;
	
	public ChdescCreateNoop(int chdesc, int owner)
	{
		this.chdesc = chdesc;
		this.owner = owner;
	}
	
	public void applyTo(SystemState state)
	{
		state.addChdesc(new Chdesc(chdesc, 0, owner, state.getOpcodeNumber()));
	}
	
	public String toString()
	{
		return "KDB_CHDESC_CREATE_NOOP: chdesc = " + SystemState.hex(chdesc) + ", owner = " + SystemState.hex(owner);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_CREATE_NOOP, "KDB_CHDESC_CREATE_NOOP", ChdescCreateNoop.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("owner", 4);
		return factory;
	}
}
