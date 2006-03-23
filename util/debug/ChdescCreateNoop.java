public class ChdescCreateNoop extends Opcode
{
	private final int chdesc, block, owner;
	
	public ChdescCreateNoop(int chdesc, int block, int owner)
	{
		this.chdesc = chdesc;
		this.block = block;
		this.owner = owner;
	}
	
	public void applyTo(SystemState state)
	{
		state.addChdesc(new Chdesc(chdesc, block, owner));
	}
	
	public String toString()
	{
		return "KDB_CHDESC_CREATE_NOOP: chdesc = " + SystemState.hex(chdesc) + ", block = " + SystemState.hex(block) + ", owner = " + SystemState.hex(owner);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_CREATE_NOOP, "KDB_CHDESC_CREATE_NOOP", ChdescCreateNoop.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("block", 4);
		factory.addParameter("owner", 4);
		return factory;
	}
}
